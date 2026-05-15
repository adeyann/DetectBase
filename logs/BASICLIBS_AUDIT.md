# BasicLibs 전수 검토

**작성일**: 2026-05-13
**대상**: /home/claudedev/DetectBase/code/BasicLibs (14,844 LOC)

## 1. 전체 구조

```
BasicLibs/
├── core/
│   ├── logger/         (~700 LOC) — MGEN 로거 + correlation_id
│   ├── metrics/        (~320 LOC) — Prometheus 메트릭 registry
│   ├── parser/
│   │   ├── json/       (~50  LOC) — nlohmann json wrapper
│   │   ├── xml/        (~5,800 LOC) ← vendored tinyxml
│   │   └── yaml-cpp/   (~3,000 LOC) ← vendored yaml-cpp + prebuilt .a
│   ├── structure/      (~670 LOC) — SafeQueue / SafeThread / ReplyDispatcher
│   └── types/          (~1,500 LOC) — InferObject 등 도메인 타입
├── profile/            (~1,200 LOC) — ServiceProfile/EngineProfile 파서
└── utils/              (~280 LOC) — file/ip/math/string/UUID 헬퍼
```

**자체 코드 약 4,700 LOC + vendored 약 8,800 LOC + 헤더-only 외부 wrapper**

## 2. 자체 유틸리티 (자체 코드)

### 2.1 core/structure — 동시성 인프라

#### `SafeQueue.h` (209 LOC, 18 파일에서 사용) ⭐ 핵심
- 템플릿 thread-safe 큐 (deque + mutex + condition_variable)
- **백프레셔 패턴**: `SetMaxSize(n)` + enqueue 시 oldest drop ([SafeQueue.h:58-60](code/BasicLibs/core/structure/SafeQueue.h#L58-L60))
- API:
  - `enqueue_copy(const T&)` / `enqueue_move(T&&)` — drop-oldest 정책 내장
  - `dequeue_wait_for(timeout)` → `std::optional<T>` (예외 제거됨, CLAUDE.md 준수)
  - `terminate()` — 종료 신호 (모든 dequeue 가 std::nullopt 반환)
  - `clear_with_action(lambda)` — 종료 시 자원 정리 (외부 catch 보호)
  - `clear_without_action()` — 빠른 비움 (swap 트릭)
- type alias: `sptrSafeQueue<T>` (11 파일), `uptrSafeQueue<T>` (2 파일)
- **CLAUDE.md 정책 준수**: 복사/이동 금지, exception 없음, RAII
- **품질 A**: 인터페이스 깔끔, 정책 명문화, 안전성 검증됨

#### `SafeThread.h` (67 LOC, 16 파일에서 사용)
- **단순 thread wrapper** — std::thread + atomic<bool> running 플래그
- API: `SetThreadFunctions(runner, closer)` + `Start()` / `Stop()`
- **스레드 풀 아님** — 각 인스턴스가 단일 thread 소유
- 사용자가 "스레드풀도 있는거 같은데"라고 했는데 **실제로는 풀 없음**
  - 37개 스레드 (4cam × 3 + 글로벌 25) 가 각자 SafeThread 인스턴스
  - 카메라당 InferenceThread + IOWorker + RTSP receive
- **품질 B+**: 명료하나 풀이 없어 카메라 N개 확장 시 오버헤드 발생 가능. PoC 후 확장 시 풀 도입 고려 항목.

#### `ReplyDispatcher.h` (260 LOC, 8 파일에서 사용)
- 비동기 reply 라우팅 (gRPC unary handler 관리)

#### `ReplyDispatcherWithCleaner.h` (132 LOC)
- 위 + 자동 cleanup 기능

### 2.2 core/logger — 관측성 인프라

#### `MgenLogger.cpp/h` (~636 LOC, **49 파일** 사용) ⭐ 가장 많이 쓰임
- 로깅 매크로 (`MLOG_INFO`, `MLOG_ERROR`, ...)
- JSON 구조화 로그 + correlation_id
- 로그 파일 회전 (logrotate 와 별개의 내부 회전 가능성)

#### `CorrelationContext.h/cpp` (~120 LOC, 5 파일 사용)
- thread-local correlation_id 컨텍스트
- 요청 traceability

### 2.3 core/metrics — Prometheus 게이지/카운터

#### `MetricsRegistry.cpp/h` (320 LOC, 11 파일 사용)
- prometheus-cpp wrapping
- `detectbase_*` 메트릭 일괄 관리
- Expose: HTTP /metrics endpoint (P54)

### 2.4 core/types — 도메인 타입

| 파일 | LOC | 사용 |
|---|---|---|
| `InferObject.cpp/h` | 752 | **26 파일** — YOLO 추론 결과 객체 (bbox/class/score) |
| `EngineStreamTypes.cpp/h` | 320 | NPU 엔진 stream 타입 |
| `ClassChecker.h` | 131 | **yaml-cpp 사용처 (유일!)** — class allow/deny list |
| `DeviceCluster.cpp/h` | ~100 | 디바이스 클러스터 (1 파일만 사용) |
| `MgenFileSystem.h` | 89 | std::filesystem wrapper |
| `MgenTypes.h` | 86 | 기본 typedef |
| `InterProtocolTypes.h` | — | 프로토콜 간 타입 |
| `ServiceStreamTypes.h` | — | 서비스 스트림 타입 |
| `AbnormalEventTypes.h` | — | 이상행동 이벤트 enum (MAIA 잔재) |

### 2.5 utils — 헬퍼

| 파일 | LOC | 외부 사용 | 내부 사용 |
|---|---|---|---|
| `string_utils.cpp/h` | ~130 | **5 파일** | — |
| `file_utils.cpp/h` | ~80 | **9 파일** | — |
| `UUIDGenerator.h` | 87 | **6 파일** | — |
| `ip_utils.cpp/h` | ~60 | **0** | NetworkProfileParser |
| `math_utils.cpp/h` | ~50 | **0** | InferObject |

→ ip_utils / math_utils는 BasicLibs **내부 helper** (외부 사용 0)

### 2.6 profile — 설정 파싱

11 파일, ~1,200 LOC. ServiceProfile / EngineProfile / NetworkProfile / ServiceBlockProfile.
가장 많이 쓰이는 건 `EngineProfile` (14 파일).

## 3. Vendored 라이브러리 — 중복/의문점

### 3.1 yaml-cpp — **중복 의심**

- **vendored**: `core/parser/yaml-cpp/` (~3,000 LOC headers + prebuilt `lib/NPU/libyaml-cpp.a`)
- **시스템**: `Dockerfile.build:37` 에 `libyaml-cpp-dev` apt 패키지 설치
- **CMakeLists.txt:37-42**: vendored `.a` 파일을 직접 링크
- 즉 시스템 패키지는 **설치만 되고 사용 안 됨** → Dockerfile에서 제거 가능
- **사용처 단 1개**: `ClassChecker.h` (YAML::LoadFile 한 번 호출)

> 💡 ClassChecker 1개 파일을 위해 3,000 LOC vendored + duplicate apt 패키지 = **과한 비용**.
> 대안: YAML 파일을 JSON으로 변환 → nlohmann::json 으로 대체 → vendored 통째 제거 가능 (8,800 LOC 중 절반 정리).

### 3.2 tinyxml — RTSP 전용

- **vendored**: `core/parser/xml/` (~5,800 LOC: tiny_xml.cpp 1,886 + tiny_xml.h 1,803 + tiny_xml_parser.cpp 1,638 + ...)
- **사용처 단 1개**: `Management/worker/src/RtspHandler.cpp` (RTSP XML config 파싱)
- 시스템 apt 패키지 (`libtinyxml-dev` 등) **설치 안 됨** → 진짜 vendored 사용

> 💡 RTSP 가 GStreamer 로 교체될 때 **함께 제거 가능**. GStreamer 는 별도 XML 의존 없음.
> = GStreamer 전환 시 추가 −5,800 LOC 효과.

### 3.3 nlohmann json — 진짜 working parser

- **시스템**: `nlohmann-json3-dev` apt 패키지 사용
- **사용처 33 파일** — 압도적으로 가장 많이 쓰임
- `BasicLibs/core/parser/json/json_impl.h`는 SioHandler 가 쓰는 wrapper (1 파일)

## 4. dead/거의-dead code 후보

| 항목 | LOC | 사용처 | 평가 |
|---|---|---|---|
| vendored yaml-cpp (.h + .a) | 3,000 | ClassChecker 1개 | 🟡 1개를 위해 과한 비용 → JSON으로 마이그레이션 검토 |
| vendored tinyxml | 5,800 | RtspHandler 1개 | 🟡 GStreamer 전환 시 자동 제거 |
| DeviceCluster | 100 | SettingManager 1개 | 🟡 거의 사용 안 됨 → 인라인화 가능 |
| AbnormalEventTypes | — | AbnormalActions 5개 + SettingData | 🟢 AbnormalActions 모듈이 쓰면 유지 (MAIA 잔재 아님) |
| 시스템 libyaml-cpp-dev apt | — | 코드 사용 0 | 🔴 **즉시 제거 가능** (vendored와 중복) |

## 5. 품질 평가 (자체 코드 4,700 LOC)

| 영역 | 점수 | 비고 |
|---|---|---|
| SafeQueue | A | 정책 명문화, 안전성, 백프레셔 |
| MgenLogger | A | 49 파일 사용, JSON+correlation_id |
| MetricsRegistry | A | Prometheus 표준 |
| InferObject | A | 도메인 핵심 타입 |
| SafeThread | B+ | 풀 없음, 단일 스레드 wrapper |
| ReplyDispatcher | B | gRPC 보조 |
| utils | B+ | string/file/UUID 잘 쓰임, ip/math 내부용 |
| profile | B+ | 설정 파싱, 설계 OK |
| ClassChecker (yaml 사용) | C | yaml-cpp 무거운데 사용량 1개 |

## 6. 정리 권장 사항 (우선순위 순)

### P1 — 즉시 가능 (PoC 무관, 작업 30분)
1. **Dockerfile.build 에서 `libyaml-cpp-dev` 제거** — 이미 vendored 쓰는데 시스템 패키지가 중복 설치되어 있음. 사용 안 됨.

### P2 — GStreamer 전환 시 자동 해결
2. **vendored tinyxml 통째로 제거** — RtspHandler 가 사라지면 사용처 0
3. **−5,800 LOC 추가 효과** (GStreamer 전환 시)

### P3 — 검토 후 (PoC 후)
4. **ClassChecker YAML → JSON 마이그레이션** + vendored yaml-cpp 제거 (−3,000 LOC)
   - YAML 파일 1개를 JSON 으로 변환하는 것은 작업 1시간 미만
5. **SafeThread → ThreadPool 도입** — 카메라 8~16대 확장 계획 있을 시
6. **DeviceCluster 인라인화** — 1개 파일 사용을 SettingManager 안으로 흡수

## 7. PoC (GStreamer 전환) 에서 활용할 BasicLibs 자산

ONVIF 페이로더 자체 구현 (200 LOC) 설계 시 사용할 도구:

| 도구 | 용도 |
|---|---|
| `SafeQueue<XmlMetadataItem>` | metadata XML 입력 큐 (백프레셔 적용) |
| `SafeThread` | 페이로더 송신 스레드 1개 |
| `MgenLogger` | 동작 로그 (`MLOG_INFO("onvif metadata sent: %zu bytes", ...)`) |
| `MetricsRegistry` | `detectbase_onvif_metadata_sent_total` 카운터 |
| `CorrelationContext` | (선택) 이벤트→메타 추적 |
| `UUIDGenerator` | RTP SSRC 생성 시 (옵션) |
| `math_utils` | timestamp 변환 (90kHz clock) |

→ **인프라는 모두 갖춰져 있다.** ONVIF 페이로더 자체 구현 위험 매우 낮음.

## 8. BasicLibs 결론

- 자체 코드 4,700 LOC는 **고품질**, CLAUDE.md 정책 잘 반영됨
- vendored 8,800 LOC 중 5,800 (tinyxml) 은 GStreamer 전환 시 자동 정리
- vendored 3,000 (yaml-cpp) 은 ClassChecker 의 YAML→JSON 마이그레이션으로 정리 가능
- **즉시 정리 가능**: Dockerfile 의 시스템 yaml-cpp 중복 (P1)
- 사용자 질문 "스레드풀": **풀은 없다**. 카메라당 SafeThread 인스턴스 분리. 확장 시 풀 도입 검토.
