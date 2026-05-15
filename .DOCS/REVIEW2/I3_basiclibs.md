# I3 — Generic BasicLibs (parser / structure / types / utils)

## §1. Why

모든 흐름이 의존하는 토대.

- **parser**: JSON / XML / YAML 파싱 (외부 라이브러리 — nlohmann/json, tinyxml, yaml-cpp).
- **structure**: 스레드 안전 큐 / 스레드 객체 / UUID 응답 매칭 디스패처. 거의 모든 worker 의 동시성 인프라.
- **types**: 도메인 타입 (InferObject, EngineStream, Cluster, AbnormalEvent 등). 흐름 사이의 데이터 계약.
- **utils**: 문자열·파일·IP·수학·UUID 헬퍼.

제거 시 잃는 것: 모든 흐름이 동작하지 않음. 특히 SafeQueue/SafeThread/InferObject 는 모든 worker·파이프라인·이벤트 흐름의 중심 자료형이므로 수정 시 광범위한 영향.

---

## §2. Roster

### Primary (I3)

| 카테고리 | 파일 |
|---|---|
| **structure** | [SafeQueue.h](../../code/BasicLibs/core/structure/SafeQueue.h), [SafeThread.h](../../code/BasicLibs/core/structure/SafeThread.h), [ReplyDispatcher.h](../../code/BasicLibs/core/structure/ReplyDispatcher.h), [ReplyDispatcherWithCleaner.h](../../code/BasicLibs/core/structure/ReplyDispatcherWithCleaner.h) |
| **types** | [MgenTypes.h](../../code/BasicLibs/core/types/MgenTypes.h), [InferObject.h](../../code/BasicLibs/core/types/InferObject.h), [InferObject.cpp](../../code/BasicLibs/core/types/InferObject.cpp), [EngineStreamTypes.h](../../code/BasicLibs/core/types/EngineStreamTypes.h), [EngineStreamTypes.cpp](../../code/BasicLibs/core/types/EngineStreamTypes.cpp), [ServiceStreamTypes.h](../../code/BasicLibs/core/types/ServiceStreamTypes.h), [InterProtocolTypes.h](../../code/BasicLibs/core/types/InterProtocolTypes.h), [DeviceCluster.h](../../code/BasicLibs/core/types/DeviceCluster.h), [DeviceCluster.cpp](../../code/BasicLibs/core/types/DeviceCluster.cpp), [AbnormalEventTypes.h](../../code/BasicLibs/core/types/AbnormalEventTypes.h), [ClassChecker.h](../../code/BasicLibs/core/types/ClassChecker.h), [MgenFileSystem.h](../../code/BasicLibs/core/types/MgenFileSystem.h) |
| **utils** | [string_utils.{h,cpp}](../../code/BasicLibs/utils/), [file_utils.{h,cpp}](../../code/BasicLibs/utils/), [math_utils.{h,cpp}](../../code/BasicLibs/utils/), [ip_utils.{h,cpp}](../../code/BasicLibs/utils/), [UUIDGenerator.h](../../code/BasicLibs/utils/UUIDGenerator.h) |
| **parser (자체)** | [json/json_impl.h](../../code/BasicLibs/core/parser/json/json_impl.h) — `filterJsonArray` 헬퍼 1개만 자체. 나머지는 외부 |
| **parser (외부 라이브러리)** | json/json.hpp + json_fwd.hpp (nlohmann/json), xml/tiny_*.{h,cpp} (TinyXML), yaml-cpp/* (yaml-cpp) — DetectBase 가 사용하는 API 표면만 §3 에서 정리 |

### Also-touches (I3 가 다른 흐름에서 어떻게 쓰이는지)

거의 모든 흐름이 I3 를 사용하므로 모든 흐름의 §2 Roster 에 "T:I3" 표기.
대표 사용처:

- **SafeQueue<T>** — F3 카메라 frame queue / engine input·output queue / F4 emit queue / F5 GRPC tag pump
- **SafeThread** — F3 detector thread / F4 IOWorker / ReplyDispatcherWithCleaner / F5 GRPC server CompletionQueue thread
- **ReplyDispatcher / WithCleaner** — F3 추론 요청-응답 매칭 (correlation_id 기반), F4 SocketIO ack 매칭(검증 필요)
- **InferObject / EngineStream*** — F3 (engine in/out) → F3 트래커 → F3 abnormal → F4 emit. 메인 파이프라인 데이터 계약
- **DeviceCluster** — F2 NetworkSettings 의 카메라 ID 집합 파싱
- **AbnormalEventTypes::EventClass** — F3 abnormal 출력 → F4 emit JSON
- **ClassChecker** — F3 추론 결과 분류
- **string/file/math/ip utils** — 모든 흐름

---

## §3. How (구조 및 사용 패턴)

### 3.1 SafeQueue<T> — MPMC 스레드 안전 큐

```
producer thread          consumer thread
  enqueue_copy/move          dequeue / dequeue_wait_for
       │                          │
       ├─ lock(m)                 ├─ wait(c, predicate)
       ├─ if size>=max_size       ├─ if terminate → throw|nullopt
       │   pop_front()  ← drop oldest
       ├─ push_back                ├─ move front, pop
       └─ notify_one              └─ return
```

- 핵심 멤버: `std::deque<T> q`, `std::mutex m`, `std::condition_variable c`, `bool b_terminate`, `size_t max_size_`
- `enqueue_copy` / `enqueue_move` 는 max_size 초과 시 **oldest drop**. P40 의 emit_queue 1000-cap drop oldest 정책의 토대.
- `dequeue()` — terminate 시 `std::runtime_error` **throw** ([SafeQueue.h:88-99](../../code/BasicLibs/core/structure/SafeQueue.h#L88-L99))
- `dequeue_wait_for(ms)` — terminate / timeout 시 `std::nullopt` 반환 (예외 없음)
- `clear_with_action(fn)` — 큐를 임시 deque 로 swap 후 락 해제 → 각 element 에 fn 적용. 락 hold 시간 최소화. fn 의 예외는 내부 try/catch 후 stderr 출력하고 다음 element 진행.

### 3.2 SafeThread — RAII thread + start/stop runner/closer 콜백

```
SafeThread t;
t.SetThreadFunctions(runner, closer);
t.Start();   // is_running_ = true → std::thread(runner)
...
t.Stop();    // is_running_ = false → closer() → join()
~SafeThread(); // Stop() 자동
```

- `is_running_` (atomic): exchange(true/false) 로 중복 Start/Stop 방지
- closer 는 runner 의 wait 를 깨우는 책임 (notify_all, queue.terminate(), socket close 등)
- 사용처(추정): ReplyDispatcherWithCleaner / SocketIO 관련 / GRPC thread 등 (다음 흐름에서 검증)

### 3.3 ReplyDispatcher<T,U> — Sharded UUID 기반 비동기 응답 매칭

```
요청측:                       응답측:
wait_and_get(uuid, ms)         set_reply(uuid, data)
  │                              │
  ├─ bucket = hash(uuid) % N     ├─ bucket = hash(uuid) % N
  ├─ table.try_emplace(uuid)     ├─ table.try_emplace(uuid)
  ├─ entry shared_ptr            ├─ entry shared_ptr
  ├─ entry.cv.wait_for(pred)     ├─ entry.data = data
  ├─ if has_value:               └─ entry.cv.notify_all
  │   bucket.table.erase(uuid)
  └─ return data
```

- `_shard_count = 16` (default). bucket-level lock 으로 전역 경합 분산
- entry 는 `std::shared_ptr<ReplyEntry>` — wait_and_get 측이 entry 를 잡고 wait 하는 동안, set_reply 가 erase 해도 안전 (shared 수명)
- timeout 시 entry 는 table 에 잔존 → ReplyDispatcherWithCleaner 가 주기적으로 `remove_expired` 호출

### 3.4 ReplyDispatcherWithCleaner<T,U>

```
StartAutoCleanup(expiration, interval)
   └─ SafeThread runner = CleanupRunner
        loop while running:
          dispatcher_.remove_expired(expiration)
          cv.wait_for(interval, until !running)
   └─ closer = CleanupCloser → cv.notify_all
StopAutoCleanup → SafeThread::Stop → closer → join
```

- 100ms 미만 interval 자동 클램프 ([ReplyDispatcherWithCleaner.h:92](../../code/BasicLibs/core/structure/ReplyDispatcherWithCleaner.h#L92))
- 소멸자에서 StopAutoCleanup 호출 → RAII

### 3.5 InferObject — 메인 파이프라인 데이터 단위

```
RKNN inference output → InferObject{engine_id, class_id, track_id, score, bbox, extend_data}
   → SORTTracker assign track_id
   → AbnormalActionChecker check
   → emit (SocketIO/GRPC)
```

- `bbox` 좌표 변환: `ConvertInferObjectCoordinate(obj, src_style, dst_style)` — pixel↔ratio, ltx↔cx, padded↔original 자동 변환 + 클램핑 ([InferObject.cpp:217-490](../../code/BasicLibs/core/types/InferObject.cpp#L217-L490))
- Rule of Five 명시 구현 (default 와 사실상 동일하나 명시적 reset)
- `std::hash<InferObject>` — engine_id, class_id, score(\*1000) 만 해시. track_id/bbox 제외. (분류·동등성 의도)

### 3.6 EngineStreamTypes — 엔진 요청/응답 계약

- `Type::UnitID` 식별자 인코딩: `base + (MAJOR_DETECTION × 10000) + (MAGIC × 1)` — 단일 분기(Detection)이므로 현재는 base_id 와 사실상 동일. 미래 확장 여지로 잔존
- `EngineStreamMetaData{requester_unit_id, requestee_engine_uuid, request_image_pixel_w/h, correlation_id}` — 추론 요청-응답 매칭 키
- `InputLayerWrapper{meta_data, requirements, image_data:shared_ptr<vector<uchar>>}` — 엔진 큐에 들어가는 단위
- `OutputLayerWrapper{meta_data, engine_handle_uuid, infer_objects}` — 엔진이 돌려주는 단위
- `EngineLinker = function<sptrSafeQueue<Output>(EngineHandleUUID, sptrSafeQueue<Input>)>` — F3 가 LoadBalancer 에 자신을 등록할 때의 콜백 시그니처

### 3.7 utils

- `MGEN::ToUpperCase(sv)`, `Convert{Camel,Pascal,Snake}Case(sv)` — string view 입력 → string 반환
- `IsValidFile / GetFileBaseName / GetAbsolutePath / ConcatPath / MakeDirectoryWhenNotExist` — `MGEN::fs` (std::filesystem 또는 experimental fallback) 사용
- `getPhysicalIPAddress()` — `getifaddrs` + 정규식으로 ^(eth|eno|ens|enp|wlan)... 매칭. lo/docker/br-/veth 제외. 첫 매칭 IPv4 반환
- `MGEN::UUID::GetGenerator()` 싱글톤 → `generate()` v4 UUID, thread_local RNG
- `are_equal_float(a,b)` — `std::numeric_limits<float>::epsilon()` 절대 비교

### 3.8 외부 parser API 표면 (DetectBase 가 사용하는 부분만)

- **nlohmann/json** ([parser/json/json.hpp](../../code/BasicLibs/core/parser/json/json.hpp)): `nlohmann::json`, `parse`, `dump`, `is_array/is_object/contains/value/get`, `value_t`, `filterJsonArray` 헬퍼 1개([json_impl.h](../../code/BasicLibs/core/parser/json/json_impl.h))
- **TinyXML** ([parser/xml/tiny_*.cpp](../../code/BasicLibs/core/parser/xml/)): RTSP profile 파싱 등 ONVIF 메시지에서 사용 추정 — F3 또는 I1 에서 검증
- **yaml-cpp** ([parser/yaml-cpp/yaml.h](../../code/BasicLibs/core/parser/yaml-cpp/yaml.h)): `YAML::LoadFile`, `Node`, `as<T>()`, `IsMap()` — ClassChecker 가 classes.yml 파싱에 사용

---

## §4. Lifetime & Ownership

| 객체 | 소유자 | 수명 |
|---|---|---|
| `SafeQueue<T>` | 사용처가 멤버로 직접 보유 또는 `sptrSafeQueue` shared 보유 | 소유자에 종속 |
| `SafeThread` | 사용처 멤버. 소멸자에서 자동 Stop+join | 소유자에 종속 |
| `ReplyDispatcherWithCleaner` | 사용처 멤버. 소멸자에서 StopAutoCleanup | 소유자에 종속 |
| `ReplyEntry` (Dispatcher 내부) | bucket.table 의 `shared_ptr`. wait_and_get 측이 stack 의 shared_ptr 로 잡고 있어 erase 후에도 wait 종료까지 살아있음 | shared_ptr refcount |
| `InferObject` | std::vector<InferObject> 안에서 value로 보유 (move 친화) | 컨테이너 종속 |
| `image_data` (InputLayerWrapper) | `shared_ptr<vector<unsigned char>>` — 큐를 통과하면서 multi-consumer 가 공유 | shared_ptr refcount |
| `MGEN::UUID` (UUIDGenerator) | 정적 싱글톤 (function-local static `shared_ptr`) | 프로세스 종료까지 |

핵심 패턴: **컨테이너/멤버 보유 + shared_ptr 통한 큐 통과**. raw new/delete 없음 — CLAUDE.md 코딩 규칙 준수.

---

## §5. Concurrency

| 컴포넌트 | 락/조건변수 | 보호 대상 | 비고 |
|---|---|---|---|
| `SafeQueue` | `std::mutex m`, `std::condition_variable c` | `q`, `b_terminate`, `max_size_` | enqueue/dequeue 모두 락 holding 짧음. predicate 로 spurious wakeup 방어 |
| `SafeQueue::clear_with_action` | mutex 락 → swap 후 즉시 release → action 은 lock 없이 | swap 패턴으로 lock 시간 최소화 | action 예외 catch → stderr 출력 후 진행 |
| `SafeThread` | `atomic<bool> is_running_` | start/stop race | exchange 로 1회만 진입 보장 |
| `ReplyDispatcher` | bucket 별 `mutex` + entry 별 `mutex` | bucket.table / entry.data | 2단 락. table 락 hold 중에 entry.cv 호출 안 함 — set_reply 에서 entry 락 → notify_all 은 락 해제 후 가능하지만 현 구현은 `notify_all` 이 entry mutex 밖에서 호출됨 ([ReplyDispatcher.h:98-102](../../code/BasicLibs/core/structure/ReplyDispatcher.h#L98-L102)) |
| `ReplyDispatcherWithCleaner` | 자체 `mtx_` + `cond_` | runner sleep | running flag 변경 후 notify → 즉시 wake 해서 정지 신호 처리 |

흐름 간 스레드 경계의 토대 — 메인 파이프라인의 모든 producer/consumer 경계에서 SafeQueue 사용.

---

## §6. Findings

### F-I3-01 — `SafeQueue::dequeue()` 가 예외를 throw 한다 (CLAUDE.md 규칙과 충돌)
- **등급**: WARN
- **위치**: [SafeQueue.h:88-99](../../code/BasicLibs/core/structure/SafeQueue.h#L88-L99)
- **내용**: terminate 시 `std::runtime_error` throw. CLAUDE.md "C++ exceptions 사용 금지 (use return values)" 규칙과 모순.
- **현 영향**: 호출부가 try/catch 로 감싸야 함 (헤더 코멘트 명시). 현재 거의 모든 사용처가 `dequeue_wait_for` (nullopt 반환) 또는 try/catch 사용으로 처리되고 있을 가능성 — F3·F4 분석에서 검증 필요.
- **권장**: 이미 `dequeue_wait_for` 가 안전한 대체이므로 `dequeue()` 사용처를 모두 wait_for 로 마이그레이션 후 deprecate. 즉시 처리 안 해도 되지만 코딩 규칙 정합성 항목.

### F-I3-02 — `SafeThread` 의 closer 미설정 시 무한 join 가능
- **등급**: WARN
- **위치**: [SafeThread.h:43-52](../../code/BasicLibs/core/structure/SafeThread.h#L43-L52)
- **내용**: runner 가 무한 루프인데 closer 가 nullptr 이면 Stop() 의 join 이 영원히 안 끝남. 소멸 시 hang.
- **현 영향**: 사용 컨벤션 의존. 모든 사용처가 closer 를 적절히 set 해야 함.
- **권장**: SetThreadFunctions 에 closer 미설정 시 ASSERT 또는 warning 로그. 또는 docs 강화. F4(IOWorker) / F5(GRPC) 분석 시 사용처 검증.

### F-I3-03 — `SafeQueue` drop oldest 시 메트릭 미수집
- **등급**: NOTE
- **위치**: [SafeQueue.h:57-58, 68-69](../../code/BasicLibs/core/structure/SafeQueue.h#L57-L58)
- **내용**: max_size 초과로 pop_front 시 silent drop. 메트릭 hook 없음.
- **현 영향**: emit_queue 등에서 drop 발생 시 외부 관측 불가. F4 분석 시 emit drop 메트릭 별도 존재 여부 확인 필요(만약 별도 카운터가 있다면 해결).
- **권장**: F4 검증 후 결정. 별도 카운터가 없다면 SafeQueue 에 optional dropped_count getter 추가 고려.

### F-I3-04 — `ReplyDispatcher::wait_and_get` timeout 시 entry 잔존 누적 가능
- **등급**: NOTE (이미 WithCleaner 가 mitigation)
- **위치**: [ReplyDispatcher.h:142-145](../../code/BasicLibs/core/structure/ReplyDispatcher.h#L142-L145)
- **내용**: timeout 으로 result 가 nullopt 일 경우 erase 안 함. WithCleaner 사용처가 아니라면 무한 누적.
- **현 영향**: 현재 사용처가 WithCleaner 인지 그냥 ReplyDispatcher 인지 확인 필요(F3·F4 분석).
- **권장**: timeout 시에도 entry 즉시 제거 옵션 추가 또는 사용처가 항상 WithCleaner 사용.

### F-I3-05 — `InferObject` Rule of Five 가 default 와 사실상 동일
- **등급**: INFO
- **위치**: [InferObject.cpp:6-79](../../code/BasicLibs/core/types/InferObject.cpp#L6-L79)
- **내용**: 명시 정의된 copy/move 가 = default 와 동일한 동작. move ctor 의 source enum reset 만 차이(POD 라 의미 없음).
- **현 영향**: 무. 단지 코드 라인 증가, 향후 멤버 추가 시 5개 다 갱신 부담.
- **권장**: = default 로 회귀해 Rule of Zero. 다만 즉시 가치 낮음.

### F-I3-06 — `InferObject` 의 hash 가 score 음수 시 size_t 캐스팅 UB 가능
- **등급**: INFO (실제 트리거 가능성 매우 낮음)
- **위치**: [InferObject.h:204-209](../../code/BasicLibs/core/types/InferObject.h#L204-L209)
- **내용**: `static_cast<size_t>(score * 1000)` — score 가 음수일 경우 부동→부호없는 정수 변환은 UB. 현 도메인에서 score 는 0~1.
- **권장**: `static_cast<size_t>(static_cast<long long>(score * 1000))` 또는 abs/clamp.

### F-I3-07 — `getPhysicalIPAddress` 가 매 호출마다 regex 컴파일
- **등급**: INFO
- **위치**: [ip_utils.cpp:39](../../code/BasicLibs/utils/ip_utils.cpp#L39)
- **내용**: lambda local regex 객체 매번 생성. 호출 빈도 낮으면 무시 가능.
- **권장**: `static const std::regex` 로 옮기면 1회 컴파일.

### F-I3-08 — `getPhysicalIPAddress` 첫 매칭만 반환 — 비결정적
- **등급**: NOTE
- **위치**: [ip_utils.cpp:23-47](../../code/BasicLibs/utils/ip_utils.cpp#L23-L47)
- **내용**: getifaddrs 순서가 OS/구성에 따라 다를 수 있음. 멀티 NIC 머신에서 결정적으로 같은 IP 가 보장 안 됨.
- **현 영향**: 단일 NIC odroid 환경에서는 사실상 안전.
- **권장**: NetworkSettings 에서 명시적 인터페이스명 지정 옵션 추가 (장기). 현재는 deferred.

### F-I3-09 — `ReplyDispatcher::terminate` 이후에도 새 entry 등록 가능
- **등급**: NOTE
- **위치**: [ReplyDispatcher.h:164-187](../../code/BasicLibs/core/structure/ReplyDispatcher.h#L164-L187)
- **내용**: terminate() 가 기존 entries 만 비우고 wakeup. 이후 set_reply / wait_and_get 가 호출되면 새 entry 가 다시 만들어짐. terminate 가 영구 차단이 아님.
- **현 영향**: 종료 시점 이후 set_reply 가 들어올 가능성이 있는 패턴이라면 메모리/CV leak 위험.
- **권장**: `b_terminated` 플래그 추가하여 set/wait 모두 거부 또는 최소한 명시적 문서화.

### F-I3-10 — `MGEN::fs` 매크로 분기는 잘 설계됨
- **등급**: INFO (긍정 발견)
- **위치**: [MgenFileSystem.h](../../code/BasicLibs/core/types/MgenFileSystem.h)
- **내용**: filesystem / experimental/filesystem / 미지원 분기 + 컴파일러 버전별 한정 + iOS/macOS 버전 한정 잘 정리.

---

## §7. Open Questions (사용자 결정 필요)

1. **F-I3-01**: SafeQueue::dequeue() 의 throw 를 wait_for 마이그레이션으로 정리할까? (정합성 항목, 즉시 가치 낮음)
2. **F-I3-02**: SafeThread closer 미설정 시 ASSERT 추가 여부?
3. **F-I3-03**: SafeQueue drop 카운터를 BasicLibs 레벨에서 추가할지, 아니면 사용처별(emit_queue 등)에 메트릭을 두는 게 더 좋을지? — F4 분석 후 결정 권장.

---

## §8. Self-Check

- [x] Primary 파일 모두 읽음 — structure 4개, types 12개, utils 9개, parser 자체 1개 (json_impl.h). 외부 라이브러리(yaml-cpp/json/tinyxml)는 표면 API 만.
- [x] §3 시퀀스에 코드 출처 표기 — SafeQueue/Dispatcher/InferObject/EngineStream 등 주요 라인 인용
- [x] §4 소유권 — 멤버 보유 / shared_ptr / static singleton 모두 근거 표기
- [x] §5 락·CV — 락 변수 정의 위치 + 보호 대상 + 패턴 명시
- [x] §6 Finding — 모두 등급 + file:line 첨부
- [x] 추측은 "검증 필요" / "추정" 으로 명시 (F-I3-03 메트릭 hook, ReplyDispatcher 사용처 등)
- [x] Also-touches 라벨 다른 흐름 Primary 와 모순 없음 — I3 가 토대이므로 다른 흐름의 P 와 겹치지 않음

**검증 결과**: PASS

**보강 필요 항목**:
- F3 분석 시: SafeQueue::dequeue() throw 사용처 카운트 → F-I3-01 영향 평가
- F3·F4 분석 시: ReplyDispatcher vs WithCleaner 사용 분포 → F-I3-04 영향 평가
- F4 분석 시: emit drop 메트릭 존재 여부 → F-I3-03 처리 결정
- F4·F5 분석 시: SafeThread closer 누락 사례 점검 → F-I3-02 영향 평가
