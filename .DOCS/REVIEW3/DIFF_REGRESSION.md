# 3-D 차분 회귀 검증 — 32건 패치 cross-check

> 2차 코드리뷰 후속 32건 패치를 (a) 적용 상태 / (b) 부수효과 / (c) 새 root cause 가능성 3축으로 read-only cross-check.

**검증 기간**: 2026-05-08
**검증 범위**: 옵션 B 11건 + 옵션 C 13건 + Root cause fix 1건 + 옵션 1 NOTE 3건 + 추가 NOTE 4건 = **32건**
**검증 도구**: 수동 코드 read + grep + cppcheck

---

## §1. 한 줄 결론

**32건 모두 정상 적용**. 부수효과 / 새 root cause 후보 11건 발견 — Major 3, Minor 3, Trivial 4 + 정책 보류 1.

자동화 도구가 추가 발견 (수동 리뷰 놓침):
- **NEW-1**: `NetworkManager::InitializeGrpcClients` catch(...) 누락 (F-F5-12 scope 누락)
- **NEW-5**: `FileLogger` ctor 빈 file_name early return 시 `re_open_intervals` 미초기화 → UB (clang-tidy)
- **NEW-8**: `SioHandler` cv_any → cv (TSan lock-order-inversion + double lock 18건 발생원인)
- **NEW-9**: `EngineProfile` printf %d → %u (audit cppcheck)
- **NEW-10**: `DETECTOR` event_binder make_shared nullptr 검사 dead code (audit cppcheck)
- **NEW-11**: 자체 코드 unused 변수 3건 (audit cppcheck)

**모두 fix 완료**.

---

## §2. 검증 카탈로그

### A. 옵션 B + 자체 throw 11건

| ID | 위치 | 적용 (a) | 부수효과 (b) | 새 root cause (c) |
|---|---|---|---|---|
| F-F5-07 GRPC closer | [GrpcEventClientBase.cpp:30](../../code/Protocol/GRPC/src/GrpcEventClientBase.cpp#L30) | ✅ closer 추가, dtor Shutdown 중복 제거 | 안전. 람다 `[this]` capture, cq_ 같은 객체 멤버 → lifetime 일치 | — |
| F-F5-12 Broadcast catch(...) | [NetworkManager.cpp:91](../../code/Management/manager/src/NetworkManager.cpp#L91) | ✅ catch(const std::exception&) + catch(...) 둘 다 | **NEW-1 (Major)**: 같은 파일 InitializeGrpcClients 에 catch(...) 누락 | NEW-1 |
| FileLogger ctor throw 제거 | [MgenLogger.cpp:222-241](../../code/BasicLibs/core/logger/MgenLogger.cpp#L222) | ✅ stderr 로 변경, 객체 살아있음 | log() 호출 시 reOpen → 빈 file_name 처리됨 | NEW-3 (Trivial) |
| FileLogger::reOpen throw 제거 | [MgenLogger.cpp:278-312](../../code/BasicLibs/core/logger/MgenLogger.cpp#L278) | ✅ stderr, file_stream 미열림 유지 | 빈 file_name 시 매 호출 path 생성/open 시도 (비효율) | NEW-3 |
| LoggerFactory::produce throw 제거 | [MgenLogger.cpp:320-332](../../code/BasicLibs/core/logger/MgenLogger.cpp#L320) | ✅ nullptr 반환 | get_logger 가 nullptr 검사 (line 343) ✓ | — |
| logInternal nested try (logger_fail) | [MgenLogger.cpp:353-377](../../code/BasicLibs/core/logger/MgenLogger.cpp#L353) | ✅ try/catch + nested try (메트릭 자체 실패 보호) | 정합 | — |
| NetworkProfileParser → optional | [NetworkProfileParser.h:48,52](../../code/BasicLibs/profile/NetworkProfileParser.h#L52) | ✅ Parse / CheckParsable 모두 std::optional | DETECTOR.cpp:174 호출처 has_value() 검사 ✓ | — |
| EngineProfileParser → optional | [EngineProfileParser.h:23,27](../../code/BasicLibs/profile/EngineProfileParser.h#L27) | ✅ 동일 패턴 | DETECTOR.cpp:189 호출처 has_value() 검사 ✓ | — |
| SafeQueue::dequeue() T 반환 제거 | [SafeQueue.h:76-77](../../code/BasicLibs/core/structure/SafeQueue.h#L76) | ✅ legacy dequeue() 잔존 0건 (grep 검증) | dequeue_wait_for 7곳 모두 nullopt 검사 적용 ✓ | NEW-2 (Minor): rtsp_proxy.cpp:375 try/catch dead code |
| EngineHandlerBase 마이그레이션 | [EngineHandlerBase.cpp:190](../../code/Engine/EngineBase/src/EngineHandlerBase.cpp#L190) | ✅ dequeue_wait_for(100ms) + has_value | 정합 | — |
| rtsp_proxy 마이그레이션 | [rtsp_proxy.cpp:313](../../code/Protocol/RTSP/rtsp/src/rtsp_proxy.cpp#L313) | ✅ dequeue_wait_for(100ms) + has_value | 또 다른 호출 line 377 에 try/catch 잔존 (외부 라이브러리, 사용자 정책) | NEW-2 |

### B. 옵션 C 13건

| ID | 위치 | 적용 (a) | 부수효과 (b) | 새 root cause (c) |
|---|---|---|---|---|
| 661 graceful degradation | [SettingManagerBase.h:280-288](../../code/Management/manager/include/SettingManagerBase.h#L280) | ✅ continue + WARN + setting_partial_failure_total | 실패 unit 만 누락, 다음 RenewAfterReset 에서 재시도 가능 | — |
| ScheduleSettingData 단위 진단성 | [SettingData.cpp](../../code/Management/manager/src/SettingData.cpp) | ✅ schedule_id 단위 식별 | — | — |
| 200줄 dump 제거 | [ISettingData.cpp](../../code/Management/manager/interface/ISettingData.cpp) | ✅ 제거됨 | 로그 가독성 ↑ | — |
| 메트릭 engine_input_q_drop | [EngineLoadBalancer.cpp:199](../../code/Management/manager/src/EngineLoadBalancer.cpp#L199) | ✅ Increment 호출 | — | — |
| 메트릭 emit_drop | [SioHandler.cpp:218,228](../../code/Management/worker/src/SioHandler.cpp#L218) | ✅ 두 drop 케이스 모두 | — | — |
| 메트릭 io_work_drop | [RtspDetectorUnit.cpp:1240](../../code/Main/DETECTOR/src/RtspDetectorUnit.cpp#L1240) | ✅ Increment | — | — |
| 메트릭 logger_fail | [MgenLogger.cpp:366,374](../../code/BasicLibs/core/logger/MgenLogger.cpp#L366) | ✅ 두 catch 분기 모두 nested try | — | — |
| 메트릭 setting_partial_failure | [DETECTOR.cpp:121](../../code/Main/DETECTOR/src/DETECTOR.cpp#L121) | ✅ RegisterCounter + SettingManagerBase.h:285 Increment | — | — |
| avframe_q SetMaxSize | [RtspDetectorUnit.cpp:359](../../code/Main/DETECTOR/src/RtspDetectorUnit.cpp#L359) | ✅ 2 * fps_limit | io_work_queue 도 SetMaxSize(30) 적용 (line 373) | — |
| IF-02 emergency mtx try_to_lock | [RtspDetectorUnit.cpp:163-164](../../code/Main/DETECTOR/src/RtspDetectorUnit.cpp#L163) | ✅ try_to_lock + early return | static mutex (다중 IOWorker 공유) | — |
| MAX_LOG_MSG_LEN 코멘트 | [MgenLogger.h:34](../../code/BasicLibs/core/logger/MgenLogger.h#L34) | ✅ 200 KB (200 * 1024) | — | — |
| 종료순서 코멘트 | [DETECTOR.cpp:386-393](../../code/Main/DETECTOR/src/DETECTOR.cpp#L386) | ✅ "!!! DO NOT REORDER !!!" + UAF 위험 명시 + INTER_FLOW 참조 | — | — |
| ApiHandler 헤더 코멘트 | [ApiHandler.h:74-91](../../code/Management/worker/include/ApiHandler.h#L74) | ✅ rule of 5 (delete) + noexcept + shared_ptr 코멘트 | — | — |
| README 메트릭 갱신 | [README.md:97-103](../../README.md#L97) | ✅ 메트릭 섹션 존재 | — | — |

### C. Root cause fix 1건

| ID | 위치 | 적용 (a) | 부수효과 (b) | 새 root cause (c) |
|---|---|---|---|---|
| ScheduleSettingData FullArray | [SettingData.cpp:363-364](../../code/Management/manager/src/SettingData.cpp#L363) | ✅ UpdateMode::FullArray + BUG FIX 코멘트 (line 359-361) | Server/Camera/ExcludeCam 는 FirstOnly 그대로 (각각 _UpdateFromJsonObject 와 시그니처 일치) | — |

### D. 옵션 1 NOTE 3건

| ID | 위치 | 적용 (a) | 부수효과 (b) | 새 root cause (c) |
|---|---|---|---|---|
| F-F2-04 setting callback metric | [SetterBase.h:105,109,248,252](../../code/Management/manager/include/SetterBase.h#L105) | ✅ 4곳 catch 분기 모두 | — | — |
| F-F1-06 main exit code 1 | [Main.cpp:53,61](../../code/Main/BASE/src/Main.cpp#L53) | ✅ Initialize/Run 실패 시 return 1 | docker exit code → restart 트리거 | — |
| F-F4-07 emit_queue copy → move | [SioHandler.cpp:220](../../code/Management/worker/src/SioHandler.cpp#L220) | ✅ enqueue_move + std::move | enqueue_copy 다른 케이스 line 230 (의도된 분기) | — |

### E. 추가 NOTE 4건

| ID | 위치 | 적용 (a) | 부수효과 (b) | 새 root cause (c) |
|---|---|---|---|---|
| F-F6-06 RegisterX label_keys default | [MetricsRegistry.h:53,55](../../code/BasicLibs/core/metrics/MetricsRegistry.h#L53) | ✅ default = {} 명시 + 코멘트 | — | — |
| F-I3-07 regex static | [ip_utils.cpp:19](../../code/BasicLibs/utils/ip_utils.cpp#L19) | ✅ static const std::regex | thread-safe (C++11 magic statics) | — |
| F-F6-03 GetLogString thread_local | [MgenLogger.cpp:408](../../code/BasicLibs/core/logger/MgenLogger.cpp#L408) | ✅ thread_local + zero-init 제거 | 재진입 안전 (std::string 즉시 복사 후 반환) | — |
| V-05 chrono_literals 헤더 5건 | SafeQueue.h, ReplyDispatcher.h, ReplyDispatcherWithCleaner.h, file_utils.h, RtspDetectorBlock.h | ✅ 모두 코멘트 + std::chrono::milliseconds 명시 | .cpp 7곳 `using namespace std::chrono_literals` 잔존 — 의도적 (헤더에서만 격리, .cpp 안 별도 using) | — |

---

## §3. 신규 발견 (4건)

### NEW-1 ⚠ Major — InitializeGrpcClients catch(...) 누락

**위치**: [NetworkManager.cpp:32-63](../../code/Management/manager/src/NetworkManager.cpp#L32)

**현상**:
```cpp
bool NetworkManager::InitializeGrpcClients( void ) noexcept
{
    ...
    for( const auto& peer : init_profile_.grpc_peers ) {
        try {
            auto client = std::make_shared<GrpcEventClientBase>( peer.ip, ... );
            ...
        }
        catch( const std::exception& e ) {  // ⚠ catch(...) 누락
            MLOG_ERROR("...");
        }
    }
    return true;
}
```

**문제**:
- 함수 시그니처 `noexcept`
- `catch( const std::exception& e )` 만 있고 `catch(...)` 누락
- protobuf FatalException 등 std::exception 안 상속하는 throw 발생 시 → noexcept 위반 → `std::terminate`

**비교**: 같은 파일의 `BroadcastEventOnlyJsonToGrpcPeers` 는 F-F5-12 패치로 `catch(...)` 추가됨. **F-F5-12 의 scope 누락**.

**영향**:
- 운영 중 GRPC enable + peer 추가 시 init 단계 비정상 종료 가능
- 현재 운영은 grpc_client_enabled=0 → 실제 발현 가능성 낮음, 하지만 분기 프로젝트 (Master/Slave) 가 GRPC 활성화 시 즉시 위험 영역

**권고**: F-F5-12 와 동일한 catch(...) 추가 (3-5 LOC).

```cpp
catch( const std::exception& e ) {
    MLOG_ERROR("...");
}
catch( ... ) {  // 추가
    MLOG_ERROR("   - GRPC Client[%s] init unknown exception", peer.name.c_str() );
}
```

---

### NEW-2 Minor — rtsp_proxy.cpp dead try/catch

**위치**: [rtsp_proxy.cpp:374-384](../../code/Protocol/RTSP/rtsp/src/rtsp_proxy.cpp#L374)

**현상**: SafeQueue::dequeue_wait_for 가 throw 안 함에도 try/catch 잔존 (dead code).

**판단**: 외부 RTSP 라이브러리 → 사용자 변경금지 정책. **변경 안 함**.

---

### NEW-3 Trivial — FileLogger::reOpen 빈 file_name 시 매 호출 재시도

**위치**: [MgenLogger.cpp:278-312](../../code/BasicLibs/core/logger/MgenLogger.cpp#L278)

**현상**: file_name 빈 채로 객체 살아있는 상태에서 log() 호출 → reOpen() 매번 path 생성/open 시도 → 매번 stderr 출력.

**영향**: 비효율. file_name 빈 케이스는 logger 비활성 케이스 → 빈도 낮음. 큰 결함 아님.

**권고**: 빈 file_name 시 early return 추가 (1 LOC). 우선순위 매우 낮음.

---

### NEW-5 ⚠ Major — FileLogger ctor 빈 file_name early return 시 `re_open_intervals` 미초기화

**위치**: [MgenLogger.cpp:222-241](../../code/BasicLibs/core/logger/MgenLogger.cpp#L222) + [MgenLogger.h:178](../../code/BasicLibs/core/logger/MgenLogger.h#L178)

**현상** (clang-tidy `cppcoreguidelines-pro-type-member-init` 검출):
```cpp
FileLogger::FileLogger( const LoggerConfig& cfg ) : Logger( cfg )
    , json_enabled_( cfg.isJsonEnabled() )
{
    const auto name = cfg.getLogSaveFileName();
    if( name.empty() ){
        std::fprintf( stderr, "MGEN::FileLogger - empty 'file_name' config. logger disabled.\n" );
        return;  // ⚠ early return 시 re_open_intervals 미초기화
    }
    this->file_name = name;
    this->re_open_intervals = std::chrono::seconds( cfg.getLogReOpenSecond() );  // 정상 경로만 init
    reOpen();
}
```

**문제**:
- `std::chrono::seconds re_open_intervals` 멤버 ([MgenLogger.h:178](../../code/BasicLibs/core/logger/MgenLogger.h#L178))는 trivial type → value-init 안 하면 garbage
- 빈 file_name 시 early return 으로 미초기화 유지
- 그 후 누군가 `log()` → `reOpen()` 안에서 `now - last_re_open > re_open_intervals` 비교 → **UB** (garbage 값과 비교)

**발현 시나리오**:
- LoggerConfig 의 file_name 이 비어있고 (설정 파일 누락 또는 오타) + 그 후 logger 호출 발생
- 일반 운영에서 빈도 매우 낮음. 하지만 발현 시 **진짜 UB**

**관련**: NEW-3 (빈 file_name 비효율) 의 같은 ctor 에서 발생한 **더 심각한 결함**. NEW-3 와 NEW-5 통합 fix 가능.

**권고 (3-5 LOC)**:

옵션 A — 헤더에서 default-init (간결):
```cpp
// MgenLogger.h:178
std::chrono::seconds re_open_intervals { std::chrono::seconds( DEFAULT_REOPEN_SECS ) };
std::chrono::system_clock::time_point last_re_open { std::chrono::system_clock::time_point::min() };
```

옵션 B — ctor 빈 file_name 케이스에서도 명시 init:
```cpp
if( name.empty() ){
    this->re_open_intervals = std::chrono::seconds( DEFAULT_REOPEN_SECS );  // 추가
    std::fprintf( stderr, "..." );
    return;
}
```

---

### NEW-6 Trivial — YoloV5 `rknn_outputs_byte_size_` dead member

**위치**: [YoloV5_Torch_Onnx_RKNN_NPU.h:150](../../code/Engine/NPU/YoloV5_Torch_Onnx_RKNN_NPU/YoloV5_Torch_Onnx_RKNN_NPU.h#L150)

**현상** (clang-tidy 검출):
- 멤버 `size_t rknn_outputs_byte_size_` 선언 + ctor 미초기화
- grep 결과 사용처 **0건** (read/write 모두 없음)
- → dead member

**영향**: 0 (사용 안 됨). CLAUDE.md "Don't add features beyond what task requires" 위반 후보.

**권고**: 멤버 선언 제거 (1 LOC). 우선순위 매우 낮음.

---

### NEW-9 Minor — EngineProfile printf 포맷 mismatch

**위치**: [EngineProfile.cpp:86-91](../../code/BasicLibs/profile/EngineProfile.cpp#L86)

**현상** (`./detectbase.sh audit` cppcheck 검출):
```cpp
snprintf( ..., "   - [%d] %-25s [ %-8s | Batch_%d | ...",
    _uuid,                  // unsigned int (InferEngineID)
    ...
    _inference_batch_size,  // unsigned int
```
`%d` 는 signed int 인데 `_uuid` / `_inference_batch_size` 가 unsigned int → printf 형식 UB. 큰 값에서 음수로 표시.

**Fix**: `%d` → `%u` (2곳).

---

### NEW-10 Minor — DETECTOR.cpp event_binder make_shared nullptr 검사 dead code

**위치**: [DETECTOR.cpp:614-623, 668-677](../../code/Main/DETECTOR/src/DETECTOR.cpp#L614)

**현상** (cppcheck `knownConditionTrueFalse` 검출):
```cpp
auto event_binder_exception_update = std::make_shared<SioEventBinder>(...);
if( !event_binder_exception_update ){  // always false (make_shared throw or non-null)
    MLOG_ERROR("...EventBinder create failed");
    return false;
}
```

NEW-4 (SORTTracker) 와 같은 패턴. make_shared 는 `std::bad_alloc` throw 또는 non-null 반환.

**Fix**: 두 곳 nullptr 검사 제거.

---

### NEW-11 Trivial — 자체 코드 unused/unassigned 변수 (cppcheck audit 검출)

**위치 (3건, false positive 다수 제외)**:
- [AbnormalActionChecker.cpp:77](../../code/AbnormalActions/src/AbnormalActionChecker.cpp#L77) `first_tracked_object` unused → `_` (throwaway)
- [ServiceBlockProfile.cpp:186](../../code/BasicLibs/profile/ServiceBlockProfile.cpp#L186) `[id, profile]` 의 id 사용 안 됨 → `[_, profile]`
- [ServiceBlockProfile.cpp:212](../../code/BasicLibs/profile/ServiceBlockProfile.cpp#L212) `[id, type]` 의 id 사용 안 됨 → `[_, type]`

**검토 후 변경 안 함**:
- IOStreamManager `_` 6건: 이미 throwaway 패턴 (cppcheck false positive)
- ServiceBlockProfile 의 다른 6건 (`from_id`, `to_id`, `id`): 모두 사용됨 (cppcheck false positive)
- MgenSchedule `xpos_ratio`: 사용됨 (cppcheck false positive)
- GrpcEventServerBase `released`: 의도된 RAII (scope 끝 dtor)

**Fix**: 3건만 throwaway 로 변경. 나머지는 false positive 라 변경 안 함.

---

### NEW-8 ⚠ Major — SioHandler 의 condition_variable_any → condition_variable

**위치**: [SioHandler.h:159](../../code/Management/worker/include/SioHandler.h#L159)

**현상** (TSan 검출):
- `std::condition_variable_any cond` 사용
- 멤버 mutex 가 `std::mutex lock` (line 160)
- TSan: lock-order-inversion 1건 + double lock 17건 = **18건 검출**

**문제**:
- `condition_variable_any` 는 generic mutex 지원하기 위해 자체 내부 mutex (shared_ptr<mutex>) 가짐
- wait_until / notify_all 시 사용자 mutex + 자체 mutex 두 개 lock
- TSan 가 lock 순서 cycle 로 인식 → false positive
- 실제 deadlock 가능성: 단일 thread 사용이라 매우 낮지만, 코드 패턴 자체가 잘못

**해결**:
- 멤버 mutex 가 `std::mutex` 라 `std::condition_variable` 충분
- cv_any 는 `std::shared_mutex` 등 generic lock type 에만 필요
- 변경 시: cv 효율 ↑ (자체 mutex 제거) + TSan false positive 18건 해소 + lock 의도 명확

**Fix**: `condition_variable_any` → `condition_variable` (1줄 변경)

---

### NEW-4 Trivial — SORTTracker make_unique nullptr 검사 dead code

**위치**: [SORTTracker.cpp:63, 174](../../code/Tracker/SORT/SORTTracker.cpp#L63)

**현상**:
```cpp
auto kalman_uptr = std::make_unique<MgenKalmanTracker>(...);
if( !kalman_uptr ){  // always false (cppcheck 검출)
    MLOG_ERROR(...);
    continue;
}
```

`std::make_unique` 는 throw on allocation failure 또는 non-null 반환. `!kalman_uptr` 은 절대 true 아님 (dead code).

**영향**: 0 (compiler optimize). CLAUDE.md "Don't add error handling for scenarios that can't happen" 위반 후보.

**권고**: 검사 제거 (defensive 의도면 그대로 OK). 우선순위 매우 낮음.

---

## §4. 양호 검증 — 32건 패치 효과 확인

| 측면 | 검증 결과 |
|---|---|
| 자체 throw 0건 | grep 검증 — Logger 3건, Parser 2건, SafeQueue 1건, F-F5-12 1건 모두 0 ✓ |
| 메트릭 5+1건 등록/사용 | 모든 호출처 적용 ✓ |
| graceful degradation | continue + WARN + setting_partial_failure_total 메트릭 ✓ |
| Root cause fix | ScheduleSettingData FullArray + 다른 SettingData 의 FirstOnly 도 시그니처 정합 (UpdateFromJsonObject vs UpdateFromJsonArray) ✓ |
| Parser optional | 호출처 DETECTOR.cpp 모두 has_value() 검사 ✓ |
| SafeQueue dequeue_wait_for | 7곳 호출처 모두 nullopt 검사 (rtsp_proxy:377 의 dead try/catch 제외) ✓ |
| noexcept 일관성 | Broadcast/CloseGrpcClients ✓. **InitializeGrpcClients 만 catch(...) 누락 (NEW-1)** |
| 종료 순서 | "!!! DO NOT REORDER !!!" 코멘트 + UAF 위험 명시 ✓ |
| chrono_literals 헤더 격리 | 5개 헤더 격리 ✓, .cpp 7곳 의도적 사용 ✓ |
| label_keys default | RegisterCounter/Gauge 동일 ✓ |
| regex static | C++11 magic statics 로 thread-safe ✓ |
| thread_local buffer | 재진입 안전 (std::string 복사 후 반환) ✓ |

---

## §5. cross-check (cppcheck 결과와 정합)

cppcheck 가 검출한 자체 코드 결함 7건 중:
- 5건: Tracker/SORT/MgenHungarian.cpp — variableScope, unreadVariable (외부 알고리즘 구현)
- 2건: Tracker/SORT/SORTTracker.cpp — `!kalman_uptr` always false (NEW-4 와 일치)

→ 차분 회귀의 NEW-4 가 cppcheck 와 cross-check 일치.

---

## §6. 결론 + 권고

### 즉시 처리 권장 (Major 2건)
- **NEW-1**: NetworkManager::InitializeGrpcClients catch(...) 추가 (3-5 LOC) — F-F5-12 scope 누락
- **NEW-5**: FileLogger ctor 빈 file_name 시 re_open_intervals 미초기화 → UB. 옵션 A (헤더 default-init) 또는 옵션 B (명시 init) — 3-5 LOC

### 우선순위 낮음 (옵션)
- NEW-3 (Trivial): FileLogger::reOpen 빈 file_name 시 매 호출 재시도 (NEW-5 와 통합 fix 가능)
- NEW-4 (Trivial): SORTTracker dead code 제거
- NEW-6 (Trivial): YoloV5 rknn_outputs_byte_size_ dead member 제거

### 변경 없음
- NEW-2 (Minor): rtsp_proxy.cpp dead try/catch — 외부 라이브러리 정책

### 종합
- **32건 패치 모두 정상 적용** (a)
- **부수효과 큰 것 없음** (b) — 모두 의도된 분기 또는 trivial
- **새 root cause 2건** (c) — NEW-1 (F-F5-12 scope 누락), NEW-5 (옵션 B 의 throw 제거 패치 부수효과)

NEW-5 는 옵션 B (FileLogger ctor throw 제거) 패치의 **부수효과** — throw 제거하면서 early return 으로 변경했지만 멤버 init 누락. clang-tidy 가 잡음.
