# DetectBase 코드 리뷰 / 작업 카탈로그

**범위**: 전체 코드베이스 (BasicLibs / VisionCommon / Tracker / Engine / Management / Protocol / AbnormalActions / Main)
**환경**: Odroid M2 aarch64, RKNN 1.5.2, C++17
**최종 갱신**: 2026-05-09 (3차 리뷰 완료, production-ready baseline 확정)

> **레거시 문서 (.DOCS/)**: 2026-05-13 시점에 logs/ 에서 .DOCS/ 로 이동된 1~3차 리뷰 통합 카탈로그.
> 다음 세션 진입점은 [logs/NEXT_SESSION.md](../logs/NEXT_SESSION.md).
> 3차 완료 시점의 baseline 스냅샷은 [REVIEW3_COMPLETION_BASELINE_20260513.md](REVIEW3_COMPLETION_BASELINE_20260513.md).
>
> **리뷰 단계별 산출물** (모두 .DOCS/ 안):
> - 1차 리뷰: 이 문서 (단계별 작업 카탈로그)
> - 2차 리뷰 (흐름 기반 9개 흐름 + 결합부 + 메타): [REVIEW2/SUMMARY.md](REVIEW2/SUMMARY.md)
> - 3차 리뷰 (자동화 도구 + 운영 시뮬레이션 + 차분 회귀): [REVIEW3/SUMMARY.md](REVIEW3/SUMMARY.md)

---

## 목차

1. [현재 진행 중 (Active)](#1-현재-진행-중-active)
2. [잔존 — 보류 (트리거 의존)](#2-잔존--보류-트리거-의존)
3. [최후의 작업 (다른 모든 작업 끝난 후)](#3-최후의-작업-다른-모든-작업-끝난-후)
4. [기각 (재검토 안 함)](#4-기각-재검토-안-함)
5. [보존 결정 (의도적으로 안 건드림)](#5-보존-결정-의도적으로-안-건드림)
6. [완료 이력 요약](#6-완료-이력-요약)
7. [Z-1 race 분석 (미래 참고용)](#7-z-1-race-분석-미래-참고용)
8. [검증 메커니즘 / 컨벤션](#8-검증-메커니즘--컨벤션)

---

## 1. 현재 진행 중 (Active)

### Debug Virtual Lines (제거 가능 임시 코드)

- 모든 카메라에 가상 schedule 2개 (LineIntrusion `99999`, VehicleIntrusion `99998`)
- 6개 라인 (가로 3 + 세로 3, two_way), 24h × 모든 요일 × 알림 1초
- 위치: [code/Main/DETECTOR/src/RtspDetectorUnit.cpp](../code/Main/DETECTOR/src/RtspDetectorUnit.cpp) 6곳 (block start/end + 주석 + 호출 부분)
- **제거 안내**: [README.md](../README.md) §"Debug 하드코딩"
- **목적**: Phase 2 (IO Worker) 검증 + 시연 + 이벤트 빈발 시뮬레이션

---

## 2. 잔존 — 보류 (트리거 의존)

**2차 리뷰 결과 trigger 도달 시 처리 항목**:

| ID | 항목 | 트리거 |
|---|---|---|
| W-06 (F-F2-02) | RenewAfterReset try/catch (nlohmann 보호) | nlohmann non-throwing API 마이그레이션 (큰 작업) |
| W-13 (F-F5-01) | server_owner_ raw ptr | Master/Slave stress 테스트 |
| F-F5-08 | GRPC peer reconnect 검증 | 운영 중 peer down 관찰 |
| F-F5-09 | GRPC max_message_size | 대용량 image RPC 사용 |
| F-I2-01 | rknn_run async (현재 sync) | 성능 최적화 |

자세한 내용은 [.DOCS/REVIEW2/FINDINGS.md](REVIEW2/FINDINGS.md) §3.

**AI 단독 진행 가능한 잠재 위험 항목 = 0건** (모두 사용자 의사 결정 / 트리거 도달 / 외부 ABI).

---

## 3. 최후의 작업 (다른 모든 작업 끝난 후)

| ID | 항목 | 비고 |
|---|---|---|
| Debug Virtual Lines 제거 | 가상 schedule 99999/99998 + helper 함수 | 시연/검증 종료 후. [README.md](../README.md) §"Debug 하드코딩" 절차 참고 |
| C4 / J7 | docker-compose healthcheck | 개발 종료 직전 |

> **사용자 명시 (2026-05-07)**: 위 두 항목은 "다른 작업 모두 끝난 뒤 최후에" 처리. 일반 잔존 보류와 분리.

---

## 4. 기각 (재검토 안 함)

| ID | 항목 | 사유 |
|---|---|---|
| Z-1 | shutdown 순서 변경 (PROGRAM QUIT 9s → 1-2s) | 9초 cost 수용 (사용자 결정). SIGSEGV 두 차례 시도 후 |
| 분석 5 Phase 1 | ClearAllUnitSubscriptions + reset | SetterBase snapshot 패턴 때문에 무효 |
| 분석 5 Phase 3 | shutdown 순서 변경 (Phase 2 매트릭스 일부) | Z-1 과 동일 |
| 분석 5 Phase 4 | RtspDetectorUnit shared_ptr + weak_ptr callback (~13h) | ROI = Z-1 차단 해제 뿐. Z-1 기각 → ROI 0 |
| P37 | Logger KST 하드코딩 → `localtime_r` | 사용자 결정 = 한국 전용 유지 |
| P60 | 환경별 (dev/prod) docker-compose 분리 | 사용자 결정 = 단일 환경 유지 |
| P66 | `time(nullptr)` ms 손실 | 5곳 모두 ms 정밀도 불필요 (날짜/요일/분/초 단위만 사용, UUID 로 파일 충돌 차단). 변경 시 외부 도구 호환성 깨질 위험 |

**예외 조건** (이 조건이 발생하면 재평가): "shutdown 9초 가 운영상 실제 문제로 판명".

---

## 5. 보존 결정 (의도적으로 안 건드림)

### 외부 라이브러리 패턴

| ID | 위치 | 패턴 |
|---|---|---|
| E-1 | MgenHungarian.cpp | BSD 라이브러리 변형 (`new[]/delete[]`) |
| E-2~E-4 | GrpcEventClientBase / GrpcUnaryHandler / GrpcEventServerBase | gRPC async tag 패턴. **GrpcUnaryHandler 의 detached thread 는 분석 6 fix 적용 (shared_ptr registry)** |
| E-5 | RtspHandler.cpp | TiXml 자체 소유권 관리 |
| E-6 | RtspHandler.cpp:122 | epoll C 인터페이스 malloc |
| E-7 | RtspHandler.cpp:160 | 외부 RTSP 라이브러리 패턴 |

### 인터페이스 안정성

| ID | 위치 | 비고 |
|---|---|---|
| M-19 | ISettingData.h | `InitConstWithoutJSON()` 가상 메서드 |
| M-20~M-22 | SettingManagerBase.h | ISettingManager 인터페이스 |
| W-10 | SettingMonitor.cpp | `Unsubscribe()` 외부 인터페이스 |

### 외부 라이브러리 보호 catch (Stage 7~9 보존)

- B-3 RtspHandler::StopRTSP, B-7 SORTTracker::TrackObjects, B-9 EngineProfileParser, B-10 NetworkProfileParser, B-13 SettingManagerBase::RenewAfterReset, SettingManager::Initialize
- ~~F-2 GrpcUnaryHandler detached thread~~ — **fix 적용 (2026-05-08)**. shared_ptr handler registry 로 UAF 차단
- sio_parser createJson 자기 재귀 (nlohmann value semantics 복잡성)

### 기타 보존

| ID | 사유 |
|---|---|
| W-19 RtspHandler 매직넘버 556 | 외부 RTSP 라이브러리 호환 |
| W-21 RtspState::Ready enum | 상태 머신 의미 유지 |
| D-9 EngineActiveContext | 미래 다중 엔진 확장 |
| M-28 `#define DEFINE` | 26곳 사용, 가독성 |
| M-30/31 SettingManager service_tag 분기 | 미래 확장 |
| L-11 AbnormalEventTypes.h 위치 | 파일 이동 광범위 |
| E-22 Detection 헤더 노출 | 함수 시그니처 영향 |
| MN-5 `using namespace MGEN;` cpp | cpp 이므로 OK |
| MN-8 `App->Quit()` int 변환 | 이미 명시적 |
| MN-11 APPLICATION_LOG_PATH 하드코딩 | docker volume 매핑 의존 |
| T-16/E-15 들여쓰기 혼합 | 코스메틱 |

---

## 6. 완료 이력 요약

> 상세 diff 는 git log. 여기는 카테고리별 상위 결과만.

### Stage 1~12 (기초 정리)

데드 코드, 코딩 표준, 잠재 버그, 재귀 제거, C-style cast → static_cast, F-1/F-3/F-4 잠재 버그 (signal handler async-signal-safe, cURL 누수, nullptr), SafeQueue throw 정리, REST 호출 패턴 변경 (`std::async` → `cURL CURLOPT_TIMEOUT`).

### Stage 13~24 (Group A/B/C/D/G 처리)

| 항목 | 결과 |
|---|---|
| EngineBase / EngineNPU CMakeLists 의 `Management` 의존 제거 | ✅ Stage 13 |
| SocketIO `CheckInterProcJsonValid` 타입 강화 + `GetAPICommandFromJson` try/catch + fallback 거부 | ✅ Stage 15 |
| Parser 호출자 try/catch (EngineProfile / NetworkProfile) | ✅ Stage 15 |
| `exclude_setting_` 읽기 mutex (race 제거) | ✅ Stage 16 |
| SocketIO `on_fail` `exit(0)` 제거 (graceful 종료) | ✅ Stage 17 |
| SocketIO Auth Token 환경변수화 (`MGEN_SIO_AUTH_TOKEN`) | ✅ Stage 17 |
| SocketIO reconnect 정책 명시 (지수 백오프 unlimited, max 30s) | ✅ Stage 17 |
| docker-compose `seccomp=unconfined` 제거 | ✅ Stage 18 |
| Log rotation 설정 (logrotate) | ✅ Stage 20 |
| `SettingMonitor::ClearAllSubscriptions` + RtspDetectorUnit dtor 호출 | ✅ Stage 14 |
| Stage 21~24 cosmetic — 함수 PascalCase, C-style cast, NULL→nullptr | ✅ |

### 후속 적용 (2026-05-07)

| 항목 | 결과 |
|---|---|
| **A-1 curlpp 완전 종결** — RTSP CMakeLists link + Dockerfile.build apt 패키지 모두 제거 | ✅ |
| **Z-2 git tag 락** — restclient-cpp `0.5.3` + sioclient `3.1.0` + RKNN `v1.5.2` + grpc `v1.30.2` | ✅ |
| **분석 5 Phase 2 (IO Worker)** — cv::imwrite 비동기. SafeQueue capacity 30 + drop oldest. 운영 검증: 1141 .jpg / 3분, DFPS 13.2~13.5 | ✅ |
| **#10 SettingManagerBase Phase 5 락 외부 callback 호출** | ✅ |
| **#11 SetterBase Update() 락 외부 callback 호출** | ✅ |
| **P38 HistoryChecker `steady_clock` 도입** (`MonotonicClock` alias) | ✅ |
| **P40 emit_queue capacity 1000 + drop oldest** ([SafeQueue.h:46-58](../code/BasicLibs/core/structure/SafeQueue.h#L46), [SioHandler.cpp:51](../code/Management/worker/src/SioHandler.cpp#L51)) | ✅ |
| **γ-2 ExtractUnitFromJson stoi try/catch** ([DETECTOR.cpp:432-438](../code/Main/DETECTOR/src/DETECTOR.cpp#L432)) — 비숫자 string skip + crash 차단 | ✅ |
| **Debug Virtual Lines 추가** (시연/검증용 임시 코드) | ✅ |
| **P53 — 구조화된 JSON 로그 + correlation_id** ([MgenLogger](../code/BasicLibs/core/logger/MgenLogger.cpp), [CorrelationContext](../code/BasicLibs/core/logger/CorrelationContext.cpp)) — `enable_json` flag (default true), thread-local correlation_id, RAII scope, 시스템 thread 진입점 부착 | ✅ |
| **P54 — Prometheus exporter** ([MetricsRegistry](../code/BasicLibs/core/metrics/MetricsRegistry.cpp), prometheus-cpp v1.3.0) — `:9090/metrics`, dfps_total / camera_count / events_total / errors_total / socketio_reconnect_total | ✅ |
| **P54-DiskGuard — /frame 디스크 다층 방어** ([RtspDetectorUnit IOWorker](../code/Main/DETECTOR/src/RtspDetectorUnit.cpp), [InferenceCounter](../code/Management/worker/src/InferenceCounter.cpp)) — L1 사전 차단 (≥90% skip + cool-down WARN), **L2-Regular** 자동 청소 (7일 retention, 1h cycle), **L2-Emergency** 비상 청소 (≥80% 시 과거 날짜 우선 / 당일만 남으면 절반 파일 삭제, 5min cool-down), L3 메트릭 (used_bytes/capacity_bytes/used_pct + skipped_total + cleanup_deleted_total + emergency_cleanup_total{type}) | ✅ |
| **P39 — DefineDefault DRY** ([SettingData.h](../code/Management/manager/include/SettingData.h)) — ServerSettingData 의 4-6 개 멤버 default 를 DefineDefault::* 로 통일. DRY 위반 차단 | ✅ (2026-05-08) |
| **P61 — 절대경로 외부화** ([RtspDetectorUnit.cpp](../code/Main/DETECTOR/src/RtspDetectorUnit.cpp), [InferenceCounter.cpp](../code/Management/worker/src/InferenceCounter.cpp), [docker-compose.yml](../docker-compose.yml), [.env](../.env)) — 코드의 hardcoded "/frame" 3곳 → DefineDefault 통일. NPU device path → `NPU_DEVICE_PATH` env var | ✅ (2026-05-08) |
| **GRPC Phase 1~7 — Master/Slave 양방향 통신 인프라** ([NetworkSettings.json](../settings/NetworkSettings.json), [NetworkProfileParser](../code/BasicLibs/profile/NetworkProfileParser.cpp), [NetworkManager](../code/Management/manager/src/NetworkManager.cpp), [GrpcUnaryHandler](../code/Protocol/GRPC/include/GrpcUnaryHandler.h), [GrpcEventServerBase](../code/Protocol/GRPC/src/GrpcEventServerBase.cpp), [GrpcEventClientBase](../code/Protocol/GRPC/src/GrpcEventClientBase.cpp), [proto](../code/Protocol/GRPC/protos/MgenProto.proto)) — Server/Client 독립 ON/OFF (4 조합), 분석 6 fix (shared_ptr registry), proto 샘플 RPC (CounterDelta/Snapshot/Heartbeat), 메트릭 (enabled/peer_count/send_success/send_failed/recv), self-loopback 검증 통과 (send=recv=68/68) | ✅ (2026-05-08) |

### 2차 코드리뷰 후속 적용 (2026-05-08, 32건 패치)

자세한 내용은 [.DOCS/REVIEW2/SUMMARY.md](REVIEW2/SUMMARY.md), [.DOCS/REVIEW2/FINDINGS.md](REVIEW2/FINDINGS.md).

| 항목 | 결과 |
|---|---|
| **자체 throw 11건 → 0건** ([SafeQueue.h](../code/BasicLibs/core/structure/SafeQueue.h), [MgenLogger.cpp](../code/BasicLibs/core/logger/MgenLogger.cpp), [NetworkProfileParser](../code/BasicLibs/profile/NetworkProfileParser.cpp), [EngineProfileParser](../code/BasicLibs/profile/EngineProfileParser.cpp)) — CLAUDE.md "C++ 예외 사용 금지" 100% 준수. Parser 2종 → optional 시그니처 변경. SafeQueue::dequeue() 함수 자체 제거 + 호출처 마이그레이션 (EngineHandlerBase + rtsp_proxy) | ✅ |
| **F-F5-07 GRPC client closer 추가** ([GrpcEventClientBase.cpp:24-29](../code/Protocol/GRPC/src/GrpcEventClientBase.cpp#L24)) — SafeThread closer 누락으로 인한 dtor hang 차단 | ✅ |
| **F-F5-12 Broadcast catch(...)** ([NetworkManager.cpp:88-90](../code/Management/manager/src/NetworkManager.cpp#L88)) — noexcept 시그니처 정합성 | ✅ |
| **661 graceful degradation** ([SettingManagerBase.h:281-292](../code/Management/manager/include/SettingManagerBase.h#L281)) — RenewAfterReset 의 break → continue. 한 unit 실패해도 다른 unit 진행. + setting_partial_failure_total 메트릭 | ✅ |
| **UpdateMode bug fix** ([SettingData.cpp:359-369](../code/Management/manager/src/SettingData.cpp#L359)) — ScheduleSettingData ctor 의 `FirstOnly` → `FullArray`. graceful degradation 으로 노출된 root cause. 모든 카메라의 실제 schedule 정상 적용 회복 | ✅ |
| **ScheduleSettingData 진단성** ([SettingData.cpp:170-204](../code/Management/manager/src/SettingData.cpp#L170)) — schedule_id 단위 명시 검증. 200줄 JSON dump → input items 카운트만 + skip 항목 식별 | ✅ |
| **메트릭 사각지대 5건 보강** — `errors_total{type="emit_drop\|io_work_drop\|engine_input_q_drop\|setting_callback\|logger_fail"}` + `setting_partial_failure_total{unit_key, unit_id}` | ✅ |
| **avframe_q SetMaxSize** ([RtspDetectorUnit.cpp:344](../code/Main/DETECTOR/src/RtspDetectorUnit.cpp#L344)) — capacity 명시 (2 × fps_limit). RTSP burst 시 메모리 보호 | ✅ |
| **IF-02 emergency cleanup mtx try_to_lock** ([RtspDetectorUnit.cpp 의 anonymous namespace](../code/Main/DETECTOR/src/RtspDetectorUnit.cpp#L154)) — 다중 IOWorker race 차단 | ✅ |
| **F-F1-04 종료 순서 코멘트 강화** ([DETECTOR.cpp:376-385](../code/Main/DETECTOR/src/DETECTOR.cpp#L376)) — UAF 위험 명시 | ✅ |
| **F-F1-06 main exit code** ([Main.cpp:51-66](../code/Main/BASE/src/Main.cpp#L51)) — Initialize/Run 실패 시 명시적 return 1 (docker exit code 정합성) | ✅ |
| **F-F4-01 ApiHandler 헤더 코멘트** ([ApiHandler.h:99-105](../code/Management/worker/include/ApiHandler.h#L99)) — `GET_or_throw_if_timeout` 의 historical name 명시 + 실제 throw 안 함 명시 | ✅ |
| **F-F2-04 setting callback 실패 메트릭** ([SetterBase.h](../code/Management/manager/include/SetterBase.h)) — Update + TriggerCallbacks catch 안에서 `errors_total{type="setting_callback"}` increment | ✅ |
| **F-F4-07 emit_queue copy → move** ([SioHandler.cpp:211-220](../code/Management/worker/src/SioHandler.cpp#L211)) — 큰 JSON payload unnecessary copy 회피 | ✅ |
| **F-F6-06 RegisterX label_keys default** ([MetricsRegistry.h:51-54](../code/BasicLibs/core/metrics/MetricsRegistry.h#L51)) — 의도 표현 인자 `= {}` default 추가 (호환성 유지) | ✅ |
| **F-I3-07 getPhysicalIPAddress regex static** ([ip_utils.cpp:18](../code/BasicLibs/utils/ip_utils.cpp#L18)) — 1회 컴파일 | ✅ |
| **F-F6-03 GetLogString thread_local** ([MgenLogger.cpp:387](../code/BasicLibs/core/logger/MgenLogger.cpp#L387)) — 매 호출 200KB 스택 zero-init 비용 회피 | ✅ |
| **V-05 chrono_literals 헤더 격리** (5 헤더: [SafeQueue.h](../code/BasicLibs/core/structure/SafeQueue.h), [ReplyDispatcher.h](../code/BasicLibs/core/structure/ReplyDispatcher.h), [ReplyDispatcherWithCleaner.h](../code/BasicLibs/core/structure/ReplyDispatcherWithCleaner.h), [file_utils.h](../code/BasicLibs/utils/file_utils.h), [RtspDetectorBlock.h](../code/Main/DETECTOR/include/RtspDetectorBlock.h)) — `using namespace std::chrono_literals` 제거. CLAUDE.md "using namespace std (in headers) 금지" 준수 | ✅ |

### 현재 빌드 산출물

- image: `detectbase:1.0` (prometheus-cpp v1.3.0 + grpc v1.30.2 포함)
- 분석 image: `detectbase:analysis` (clang-tidy + cppcheck + clang) — [Dockerfile.analysis](../Dockerfile.analysis)
- 마지막 lifecycle 검증: DFPS 52.4, 4 cam, ERROR/WARN 0, PROGRAM QUIT SUCCESS (10초 graceful)
- 외부 의존 라이브러리 (모두 컨테이너 source build, tag 락):
  - RKNN runtime `v1.5.2` / restclient-cpp `0.5.3` / sioclient `3.1.0` / grpc `v1.30.2` / prometheus-cpp `v1.3.0`

### 심층 분석 결과 (분석 1~6 / α~θ / A~J)

처리된 카테고리: 분석 1 (Engine 거짓 의존), 4 (URL fallback stoi crash), **6 (GRPC F-2 detached thread UAF)**, α (설정 데이터 흐름), β (메모리/소유권), γ (외부 입력 검증), δ (동기화), G (network resilience), C (빌드/외부 의존), D (logging + Prometheus), E (time + steady_clock), F (설정/기본값 + DRY), J (컨테이너/배포/보안), I (명명/DRY).

분석 3 (Management → SocketIO PUBLIC 의존) = **PUBLIC 정당** 으로 결론. 큰 facade 화는 단기 이득 적음.

### 3차 코드리뷰 후속 적용 (2026-05-09, 10건 패치)

자세한 내용은 [.DOCS/REVIEW3/SUMMARY.md](REVIEW3/SUMMARY.md), [.DOCS/REVIEW3/DIFF_REGRESSION.md](REVIEW3/DIFF_REGRESSION.md), [.DOCS/REVIEW3/AUTOMATED_AUDIT.md](REVIEW3/AUTOMATED_AUDIT.md), [.DOCS/REVIEW3/RUNTIME_BEHAVIOR.md](REVIEW3/RUNTIME_BEHAVIOR.md).

**3차 리뷰 = 자동화 도구 audit + 운영 시뮬레이션 + 차분 회귀 (A + B + D 통합)**.
재실행 가능: `./detectbase.sh audit` (cppcheck + clang-tidy + ASan + UBSan), `--with-tsan` 추가 시 TSan 도.

| 항목 | 결과 |
|---|---|
| **NEW-1 InitializeGrpcClients catch(...) 추가** ([NetworkManager.cpp:61-64](../code/Management/manager/src/NetworkManager.cpp#L61)) — F-F5-12 패치의 scope 누락 (Major) | ✅ |
| **NEW-5 FileLogger re_open_intervals 헤더 default-init** ([MgenLogger.h:178-181](../code/BasicLibs/core/logger/MgenLogger.h#L178)) — 빈 file_name early return 시 미초기화 멤버 비교 UB 차단 (clang-tidy 검출, Major) | ✅ |
| **NEW-3 reOpen 빈 file_name early return** ([MgenLogger.cpp:280](../code/BasicLibs/core/logger/MgenLogger.cpp#L280)) — NEW-5 통합 (Trivial) | ✅ |
| **NEW-8 SioHandler condition_variable_any → condition_variable** ([SioHandler.h:159-163](../code/Management/worker/include/SioHandler.h#L159)) — TSan lock-order-inversion + double lock 18건 false positive 발생원인 cv_any 패턴 fix (Major) | ✅ |
| **NEW-7 CMakeLists generator expression 분리** ([CMakeLists.txt:33-37](../code/CMakeLists.txt#L33)) — `$<$<CONFIG:Debug>:-O0 -g>` 한 문자열 → 별도 옵션 분리. ASan/UBSan/TSan 빌드 가능 (Build) | ✅ |
| **NEW-4 SORTTracker make_unique nullptr 검사 제거** ([SORTTracker.cpp:55-67, 161-172](../code/Tracker/SORT/SORTTracker.cpp#L55)) — make_unique 는 throw 또는 non-null 반환. dead code (Trivial) | ✅ |
| **NEW-6 YoloV5 rknn_outputs_byte_size_ dead member 제거** ([YoloV5_Torch_Onnx_RKNN_NPU.h:148](../code/Engine/NPU/YoloV5_Torch_Onnx_RKNN_NPU/YoloV5_Torch_Onnx_RKNN_NPU.h#L148)) — 사용처 0건 (Trivial) | ✅ |
| **NEW-9 EngineProfile printf %d → %u** ([EngineProfile.cpp:86-87](../code/BasicLibs/profile/EngineProfile.cpp#L86)) — InferEngineID/_inference_batch_size unsigned int 인데 %d 사용 → 포맷 UB. audit cppcheck 검출 (Minor) | ✅ |
| **NEW-10 DETECTOR event_binder make_shared nullptr 검사 제거** ([DETECTOR.cpp:614-624, 668-677](../code/Main/DETECTOR/src/DETECTOR.cpp#L614)) — make_shared 는 throw 또는 non-null. dead code 2곳. audit cppcheck 검출 (Minor) | ✅ |
| **NEW-11 자체 코드 unused 변수 3건** ([AbnormalActionChecker.cpp:77](../code/AbnormalActions/src/AbnormalActionChecker.cpp#L77), [ServiceBlockProfile.cpp:186, 212](../code/BasicLibs/profile/ServiceBlockProfile.cpp#L186)) — structured binding `_` throwaway 로 변경. audit cppcheck 검출 (Trivial). 다른 false positive 위치는 변경 안 함 | ✅ |

### 트리거 도달 항목 처리 (2026-05-09, 2건 + OPERATIONS.md)

| 항목 | 결과 |
|---|---|
| **W-13 server_owner_ raw → weak_ptr** ([GrpcEventServerBase.h:18-21](../code/Protocol/GRPC/include/GrpcEventServerBase.h#L18), [GrpcUnaryHandler.h](../code/Protocol/GRPC/include/GrpcUnaryHandler.h), [DETECTOR.h:73](../code/Main/DETECTOR/include/DETECTOR.h#L73), [DETECTOR.cpp:312](../code/Main/DETECTOR/src/DETECTOR.cpp#L312)) — `enable_shared_from_this` 상속 + `unique_ptr` → `shared_ptr` + `weak_from_this()` capture + `TryUnregister()` helper. server destroy 후 detached thread 의 dangling 차단 (Major) | ✅ |
| **W-06 RenewAfterReset nlohmann 보호** ([SettingData.cpp:174~](../code/Management/manager/src/SettingData.cpp#L174)) — `.get<T>()` 호출 5곳에 type check (`is_number_integer`, `is_string`, `is_array`, `is_number`) 추가. 잘못된 type 입력 시 throw 대신 skip + WARN. RenewAfterReset try/catch 안전망 유지 (Major) | ✅ |
| **OPERATIONS.md 작성** ([OPERATIONS.md](../OPERATIONS.md)) — 운영자용 가이드 (cheat sheet / 메트릭 임계점 / ERROR-WARN 대응 / 트러블슈팅 / 디스크 방어 / Prometheus alert / 운영 체크리스트) | ✅ |
| **clang-tidy 100% 동작** ([detectbase.sh:audit](../detectbase.sh)) — cmake configure → fresh compile_commands.json (path 일치) → 51 파일 모두 정상 분석. diagnostic-error 64 → 0건. 신규 검출 modernize-use-nullptr 140건 + member-init 6건 등 (외부 tinyxml/RTSP 라이브러리는 정책상 보존) | ✅ |
| **NEW-12 RtspHandler nullptr 5건** ([RtspHandler.cpp:64,109,123,175,176](../code/Management/worker/src/RtspHandler.cpp#L64)) — `time(NULL)`, `== NULL`, `sys_os_create_thread(..., NULL)` 등 5곳 nullptr 로 변경. clang-tidy `modernize-use-nullptr` 검출. line 122 의 C-style cast 는 [E-6 외부 라이브러리 보존 정책](#5-보존-결정-의도적으로-안-건드림) 에 따라 변경 안 함 | ✅ |

### 48h 운영 안정성 검증 + W-14 (2026-05-09 ~ 진행 중)

| 항목 | 결과 |
|---|---|
| **48h 1차 테스트** (W-14 없음, [.DOCS/TEST_48H_20260509_LEAK_FOUND.md](TEST_48H_20260509_LEAK_FOUND.md), raw data in .deleted_backup/) | 첫 38h 안정. **마지막 10h RssAnon +47MB 증가** 검출 |
| **원인 분석** | RssFile 변화 없음 + RssAnon 증가 → cache 아닌 heap 누적. 코드 검토 결과 emergency cleanup 의 std::filesystem path string 할당/해제 burst → glibc ptmalloc fragmentation 추정 |
| **W-14 malloc_trim 적용** ([RtspDetectorUnit.cpp:217-220](../code/Main/DETECTOR/src/RtspDetectorUnit.cpp#L217)) — emergency cleanup 후 `malloc_trim(0)` 호출 + `<malloc.h>` include. heap arena 를 OS 에 강제 반환 | ✅ |
| **48h 2차 테스트** (W-14 적용) | 진행 중 ([test_48h_20260512_094418](test_48h_20260512_094418/)). 2026-05-14 09:44 종료 예정. 결과 따라 jemalloc 적용 결정 |

**3차 리뷰 검증 결과**:
- 자체 코드 ASan/UBSan 검출 0건 (외부 librknnrt.so init leak 만)
- 자체 코드 TSan 진짜 race 0건 (NEW-8 fix 후 187건 모두 false positive)
- 자체 코드 cppcheck 결함 0건 (Tracker SORT 외부 알고리즘 7건만)
- 자체 코드 C-style cast 0건
- 운영 12분 측정 메모리/FD/Thread leak 없음
- 100x parallel curl /metrics 100% 200 OK
- graceful shutdown 10초 → PROGRAM QUIT SUCCESS

**production-ready baseline 확정**.

---

## 7. Z-1 race 분석 (미래 참고용)

> 진행 안 함. 위 §3 기각 참고. 본 섹션은 트리거 발생 시 재평가 자료.

cpp-reviewer 비판적 검토 (2026-05-07) 로 식별된 3가지 독립 race path:

### 1. `SetterBase::Update()` 의 callback snapshot 패턴

- 위치: [code/Management/manager/include/SetterBase.h:88-106](../code/Management/manager/include/SetterBase.h#L88)
- 문제: lock 잡고 `callbacks_to_invoke` vector 로 callback 복사 → unlock → lock 외부 invoke
- `ClearAllUnitSubscriptions()` 가 `SetterBase::callbacks_` map 을 비워도, 이미 복사된 vector 는 영향 받지 않음 → race 잔존
- 두 번째 시도 SIGSEGV 의 직접 원인

### 2. `proxy_ptr_` raw pointer dangling

- 위치: [code/Main/DETECTOR/src/RtspDetectorUnit.cpp:286, 643](../code/Main/DETECTOR/src/RtspDetectorUnit.cpp#L286) — `proxy_ptr_->runCallBacks()` 호출
- `proxy_ptr_` = `rtsp_handler_->GetProxyPtr(id_)` 로 얻은 raw pointer. CRtspProxy 객체에 대한 비소유 ref
- Network close 시 CRtspProxy 소멸 → inference thread 가 그 후 `proxy_ptr_->runCallBacks()` 접근하면 즉시 UAF
- SettingMonitor 와 완전 독립 — 새 shutdown 순서 (Network 먼저) 단독 적용 시 무조건 발생

### 3. `sio::client::sync_close()` 의 보장 범위

- 어셈블리 분석: io_service worker thread `join()` 만 수행
- 이미 post 된 socket event callback 이 별도 asio post chain 에서 실행 중일 수 있음
- sync_close() 반환 후에도 callback 실행 가능 → SettingMonitor 외 다른 callback path 도 위험

### 진짜 fix 조건 (모두 충족해야)

- `SetterBase::Update()` 의 callback invoke 시점에 weak_ptr 기반 validity check
- `proxy_ptr_` 를 raw pointer 대신 weak_ptr 또는 access 전 running flag 체크
- RtspDetectorUnit 자체를 `unique_ptr` → `make_shared` + `enable_shared_from_this` 로 lifetime 모델 변경
- = **Phase 4 큰 리팩터링 (~13h)**

---

## 8. 검증 메커니즘 / 컨벤션

### 회귀 검증 절차

```bash
# 1. 빌드
./detectbase.sh compile
# 기준: 1m20-46s, [v] Build script completed successfully!

# 2. 서비스 시작
./detectbase.sh start

# 3. 운용 검증 (로그 확인)
tail logs/DetectBase.log
# 기준:
#   ###. SERVICE START SUCCESS
#   CAM<XXX> Decoder Opened (4대 모두)
#   [DFPS] 13.0~14.1 FPS/cam (TotalDFPS 53~56)

# 4. 종료 검증
docker stop detectbase_service
# 기준:
#   PROGRAM QUIT START
#   PROGRAM QUIT SUCCESS
#   exit code 0
```

### 컨벤션

- **휴지통**: `/home/claudedev/DetectBase/.deleted_backup/` (rm 금지, mv 만)
- **로그**: `/home/claudedev/DetectBase/logs/`
- **빌드/시작/종료**: `./detectbase.sh compile|start|stop`
- **외부 라이브러리 보호 catch**: 보존 (Stage 7~8 가이드)
- **GRPC 모듈**: 보존 (다른 프로젝트 베이스)
- **사용자 직접 결정**: 광범위 리팩터링, 인프라 정책
