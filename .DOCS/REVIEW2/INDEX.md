# 2차 코드리뷰 — INDEX

> 1차는 전수 분석 + 트리거 의존 항목 식별 (완료, [CODE_REVIEW_SUMMARY.md](../CODE_REVIEW_SUMMARY.md)).
> 2차는 흐름 단위 심층 분석. 모든 소스 파일이 누락 없이 흐름에 포함된다.

---

## 진행 단계 (B 컴펌 정책)

| 단계 | 흐름 | 상태 | 산출물 |
|---|---|---|---|
| **Stage I** (Foundation) | I3 → I2 → I1 | ✅ 완료 | [I3_basiclibs.md](I3_basiclibs.md), [I2_rknn_headers.md](I2_rknn_headers.md), [I1_rtsp_core.md](I1_rtsp_core.md) |
| Stage F-base | F6 → F2 → F1 | ✅ 완료 | [F6_observability.md](F6_observability.md), [F2_configuration.md](F2_configuration.md), [F1_lifecycle.md](F1_lifecycle.md) |
| Stage F-runtime | F3 → F4 → F5 | ✅ 완료 | [F3_pipeline.md](F3_pipeline.md), [F4_event_output.md](F4_event_output.md), [F5_grpc.md](F5_grpc.md) |
| Stage Synthesis | INTER_FLOW + FINDINGS | ✅ 완료 | [INTER_FLOW.md](INTER_FLOW.md), [FINDINGS.md](FINDINGS.md) |
| Stage ABCD (재진입) | B → A → C → D | ✅ 완료 | [DEFERRED_VERIFICATION.md](DEFERRED_VERIFICATION.md), [SYSTEM_VIEW.md](SYSTEM_VIEW.md), [META_REVIEW.md](META_REVIEW.md), [SUMMARY.md](SUMMARY.md) |
| Stage VERIFICATION_FINAL | 빌드/보안/테스트/정적분석/결합부 매트릭스 | ✅ 완료 | [VERIFICATION_FINAL.md](VERIFICATION_FINAL.md) |
| **Stage 후속 패치** (옵션 B/C/1/NOTE 4) | 자체 throw 11건 + 옵션 C 13건 + UpdateMode root cause + NOTE 7건 | ✅ 완료 (32건 패치, ~210 LOC) | [CODE_REVIEW_SUMMARY.md §6](../CODE_REVIEW_SUMMARY.md) |
| **3차 리뷰** (자동화 도구 + 운영 + 차분 회귀) | 추가 결함 발견 + fix | ✅ 완료 (7건 fix, 누적 39건) | [REVIEW3/SUMMARY.md](../REVIEW3/SUMMARY.md) |

> **단일 진입점**: [SUMMARY.md](SUMMARY.md) 1개 문서로 전체 리뷰 결과 파악 가능.

**최종 상태**: production-ready baseline. AI 단독 처리 가능 항목 0건. 잔존은 모두 트리거 의존 / 사용자 결정 / 외부 ABI / 분기 task.

각 단계 종료 시점에 사용자 컴펌. 흐름·단계마다 self-check.

---

## 흐름 정의 (9개 + 결합부)

| ID | 이름 | 한 줄 목적 |
|---|---|---|
| F1 | Bootstrap & Shutdown Lifecycle | 프로세스 진입~종료 |
| F2 | Configuration & Live Reload | 설정 파싱·반영·콜백 |
| F3 | Camera Pipeline | RTSP→NPU→Tracking→Abnormal |
| F4 | Event Output | SocketIO/REST/IOWorker outbound |
| F5 | GRPC Bidirectional | server+client |
| F6 | Observability | Logger + Metrics + Disk Defense |
| I1 | RTSP/HTTP/RTP Proxy & Server Core | 외부 라이브러리 |
| I2 | RKNN API Headers | 외부 헤더 |
| I3 | Generic BasicLibs | parser/structure/types/utils |
| INTER_FLOW | 흐름 간 결합부 | 흐름 사이 계약·경계 |

---

## N:M 매핑 (Primary / Also-touches)

> 각 행: 파일 → Primary(P) | Also-touches(T)
> 분석 진행 중 채워나간다. 흐름 §2 Roster 와 동기화.

### Stage I — Foundation

#### I3 — Generic BasicLibs (분석 완료, [I3_basiclibs.md](I3_basiclibs.md))

| 파일 | Primary | Also-touches |
|---|---|---|
| structure/SafeQueue.h | I3 | F3, F4, F5, F2 |
| structure/SafeThread.h | I3 | F3, F4, F5, F6 |
| structure/ReplyDispatcher.h | I3 | F3, F4 (검증) |
| structure/ReplyDispatcherWithCleaner.h | I3 | F3, F4 (검증) |
| types/MgenTypes.h | I3 | 모든 흐름 |
| types/InferObject.{h,cpp} | I3 | F3, F4, F5 |
| types/EngineStreamTypes.{h,cpp} | I3 | F3 |
| types/ServiceStreamTypes.h | I3 | F4 |
| types/InterProtocolTypes.h | I3 | F2, F4 (SocketIO 핸들러) |
| types/DeviceCluster.{h,cpp} | I3 | F2 |
| types/AbnormalEventTypes.h | I3 | F3, F4 |
| types/ClassChecker.h | I3 | F3 |
| types/MgenFileSystem.h | I3 | F4(IOWorker), F6(DiskGuard) |
| utils/string_utils.{h,cpp} | I3 | 모든 흐름 |
| utils/file_utils.{h,cpp} | I3 | F4, F6 |
| utils/math_utils.{h,cpp} | I3 | F3 (좌표 비교) |
| utils/ip_utils.{h,cpp} | I3 | F4, F5 |
| utils/UUIDGenerator.h | I3 | F3, F4 (correlation/매칭) |
| parser/json/json_impl.h (자체) | I3 | F2 |
| parser/json/{json.hpp, json_fwd.hpp} (외부) | I3 | 모든 흐름 |
| parser/xml/tiny_*.{h,cpp} (외부) | I3 | I1 (RTSP/ONVIF) |
| parser/yaml-cpp/* (외부) | I3 | F3 (ClassChecker) |

**Findings**: WARN 2건 (dequeue throw, SafeThread closer hang), NOTE 4건, INFO 4건. 자세한 내용 [I3_basiclibs.md §6](I3_basiclibs.md#6-findings)

#### I2 — RKNN headers (분석 완료, [I2_rknn_headers.md](I2_rknn_headers.md))

| 파일 | Primary | Also-touches |
|---|---|---|
| librknn_api/include/rknn_api.h | I2 | F3 (RKNN engine) |
| librknn_api/include/rknn_matmul_api.h | I2 | (DetectBase 미사용) |
| librknn_api/aarch64/lib*.so | I2 | F3 — 단, 호스트 mount 본 사용 |

**Findings**: NOTE 3건 (sync run, host so vs bundled so, version mismatch UB risk).

#### I1 — RTSP/HTTP/RTP core (분석 완료, [I1_rtsp_core.md](I1_rtsp_core.md))

| 영역 | Primary | Also-touches |
|---|---|---|
| Protocol/RTSP/core/* | I1 | (라이브러리 내부) |
| Protocol/RTSP/http/* | I1 | (라이브러리 내부) |
| Protocol/RTSP/media/* | I1 | (라이브러리 내부, 대부분 DetectBase 미사용) |
| Protocol/RTSP/rtp/* | I1 | (라이브러리 내부) |
| Protocol/RTSP/rtsp/* | I1 | F3, F4, F2 (rtsp_srv/cfg/proxy/timer 표면만 노출) |

DetectBase 가 직접 사용하는 라이브러리 진입 표면: **15개 심볼 / 5개 헤더 그룹**. 38,643 라인의 라이브러리 내부는 검토 범위 밖.

**Findings**: NOTE 2건 (thread-safety, 전역 hrtsp 단일 인스턴스), INFO 4건.

---

## Stage I 종합 self-check

| 항목 | 상태 |
|---|---|
| I3 / I2 / I1 모두 §8 self-check PASS | ✅ |
| Primary 누락 검증 (Stage I 범위) | ✅ — BasicLibs 28개 + RKNN 헤더 2개 + RTSP 라이브러리 전체. F-stage 의 다른 흐름이 사용할 수 있는 모든 인프라 포함 |
| 단계 내 흐름 간 가정 정합성 | ✅ — I3 의 SafeQueue / SafeThread 패턴이 I1 의 RTSP 라이브러리 thread 와 별도이지만 충돌 없음. RKNN(I2)는 I3 와 분리. 상호 충돌 없음 |
| RISK 등급 finding | 0건. WARN 2건 (F-I3-01 dequeue throw, F-I3-02 SafeThread closer hang) — 트리거 의존, 즉시 처리 불요 |
| INDEX 갱신 | ✅ |

**Stage I 결과**: PASS. 사용자 컴펌 대기.

### Stage F-base

#### F6 — Observability (분석 완료, [F6_observability.md](F6_observability.md))

| 파일 | Primary | Also-touches |
|---|---|---|
| logger/MgenLogger.{h,cpp} | F6 | 모든 흐름 |
| logger/MgenLoggerMacro.{h,cpp} | F6 | F1 (STEP_* 매크로) |
| logger/CorrelationContext.{h,cpp} | F6 | F4 (SocketIO 진입점), F3 (카메라 thread), F5 (GRPC 진입점) |
| metrics/MetricsRegistry.{h,cpp} | F6 | F1 (Init/Shutdown), F3·F4·F5·F6 (각 측정점) |

Disk Defense 코드는 RtspDetectorUnit.cpp 안에 임베디드 → F3 의 Primary 이지만 F6 책임 (also-touches).

**Findings**: WARN 2건 (`get_logger` 첫 호출 config 고착, FileLogger throw), NOTE 4건, INFO 5건.

#### F2 — Configuration & Live Reload (분석 완료, [F2_configuration.md](F2_configuration.md))

| 영역 | Primary | Also-touches |
|---|---|---|
| profile/* (parsers + builder) | F2 | F1 (Service ctor / Init Stage #01) |
| manager/interface/* | F2 | F2 자체 |
| manager/include/{Setter,SettingManager}Base.h | F2 | F3 (운영 객체가 callback 등록) |
| manager/include/SettingManager.h + .cpp | F2 | F1 Stage #04 (NetworkManager::ConnMVAS 내부에서 호출 추정) |
| manager/include/SettingData.h + .cpp | F2 | F3, F4 |
| worker/SettingMonitor.{h,cpp} | F2 | F3 (RtspDetectorUnit 상속) |

**Findings**: WARN 2건 (try/catch ApiHandler throw, RenewAfterReset throw), NOTE 4건, INFO 3건.

#### F1 — Bootstrap & Shutdown Lifecycle (분석 완료, [F1_lifecycle.md](F1_lifecycle.md))

| 파일 | Primary | Also-touches |
|---|---|---|
| Main/BASE/src/Main.cpp | F1 | F6 (InitLogger 호출) |
| Main/BASE/include/InitMain.h | F1 | (헤더 only, 경로 헬퍼) |
| Main/DETECTOR/include/DETECTOR.h | F1 | F3, F4, F5 (멤버로 보유) |
| Main/DETECTOR/src/DETECTOR.cpp | F1 | F2 (Initialize 시퀀스), F3 (engines/block), F4 (NetworkManager), F5 (GrpcEventServerBase), F6 (메트릭 등록 18개) |

**Findings**: WARN 2건 (Parser throw, 종료순서 컴파일 강제 부재), NOTE 5건, INFO 4건.

---

## Stage F-base 종합 self-check

| 항목 | 상태 |
|---|---|
| F6 / F2 / F1 모두 §8 self-check PASS | ✅ |
| Primary 누락 검증 | ✅ — Logger 6 + Metrics 2 + CorrelationContext 2 + profile 6 + manager 17 + Main 4 = 37개 모두 포함 |
| 단계 내 흐름 간 가정 정합성 | ✅ — F6 의 logger/metrics 가 F1 Init 에서 사용됨 / F2 의 SettingMonitor 가 F3 상속용으로 정의 / F1 의 Quit 순서가 F2 의 SettingMonitor lifecycle 과 정합 |
| RISK 등급 finding | 0건. WARN 6건 (모두 try/catch + CLAUDE.md 규칙 트레이드오프 또는 종료순서 보호) — 즉시 처리 안 함 |
| INDEX 갱신 | ✅ |

**Stage F-base 결과**: PASS. 사용자 컴펌 대기.

### Stage F-runtime

#### F3 — Camera Pipeline (분석 완료, [F3_pipeline.md](F3_pipeline.md))

| 영역 | Primary | Also-touches |
|---|---|---|
| Main/DETECTOR/RtspDetectorBlock + Unit | F3 | F1, F2, F4, F6 |
| worker/RtspHandler.{h,cpp} | F3 | F2 (cfg xml maker), I1 |
| VisionCommon/* | F3 | I3 |
| Engine/* (Base + NPU + RKNN) | F3 | I2 |
| manager/EngineLoadBalancer + IOStreamManager | F3 | F4 (IOStreamManager 가 GRPC 큐도 보유) |
| worker/EngineClient + InferenceCounter | F3 | F6 (DFPS 메트릭) |
| Tracker/SORT + TrackerBase | F3 | I3 |
| AbnormalActions/* | F3 | F4 (이벤트 emit 트리거) |

**Findings**: WARN 2건 (avframe_q_ capacity, RTSP unit lifecycle), NOTE 7건, INFO 6건.

#### F4 — Event Output (분석 완료, [F4_event_output.md](F4_event_output.md))

| 영역 | Primary | Also-touches |
|---|---|---|
| manager/NetworkManager.{h,cpp} | F4 | F1, F2, F3, F5 (grpc_clients_) |
| worker/SioHandler.{h,cpp} | F4 | F2 (inbound binder), F3 (Emit), F6 |
| worker/ApiHandler.{h,cpp} | F4 | F2 (REST GET) |
| Protocol/SocketIO/* | F4 (외부 라이브러리) | I3 |
| Protocol/REST/* + curl/* | F4 (외부 라이브러리) | I3 |

**Findings**: WARN 2건 (GET_or_throw 네이밍, emit_queue drop 메트릭), NOTE 6건, INFO 4건.

#### F5 — GRPC Bidirectional (분석 완료, [F5_grpc.md](F5_grpc.md))

| 영역 | Primary | Also-touches |
|---|---|---|
| Protocol/GRPC/include/* + src/* | F5 | F1 (server unique_ptr), F4 (client vector), F6 (메트릭 6종) |
| Protocol/GRPC/protos/* (생성) | F5 | (proto-generated) |

**Findings**: WARN 2건 (server_owner_ raw ptr, raw new/delete), NOTE 6건, INFO 5건.

---

## Stage F-runtime 종합 self-check

| 항목 | 상태 |
|---|---|
| F3 / F4 / F5 모두 §8 self-check PASS | ✅ |
| Primary 누락 검증 | ✅ — F3 의 ~30 파일 + F4 의 NetworkManager + handler 3종 + 외부 SocketIO/REST + F5 의 GRPC 8 파일 = 모든 runtime 파일 포함 |
| 흐름 간 가정 정합성 | ✅ — F3 의 emit 트리거 → F4 SocketIO + F5 GRPC. F4 NetworkManager 가 모든 outbound handler 통합. F5 의 server lifetime 가 F1 Quit 순서 #00 으로 보호 |
| RISK 등급 finding | 0건. WARN 6건 (대부분 검증/정합성 항목, 즉시 처리 안 함) |
| INDEX 갱신 | ✅ |

**Stage F-runtime 결과**: PASS. 사용자 컴펌 대기.

---

## 흐름 산출물 템플릿

```
§1. Why         — 흐름 존재 이유 (제거 시 잃는 것)
§2. Roster      — Primary 파일 / Also-touches 파일 (출처 흐름 표시)
§3. How         — 진입점→종착점 호출 시퀀스 (스레드 경계 표기)
§4. Lifetime & Ownership — 소유권 / 수명 / 소멸 순서
§5. Concurrency — 락·큐·재진입성·경계
§6. Findings    — 등급(INFO/NOTE/WARN/RISK) + 코드 출처
§7. Open Questions — 사용자 결정 필요 항목
§8. Self-Check  — 자체 검증 체크리스트 + PASS/FAIL
```

---

## Findings 등급 정의

| 등급 | 의미 |
|---|---|
| INFO | 단순 메모 |
| NOTE | 설계 의도 기록. 액션 없음 |
| WARN | 잠재 문제. 트리거 조건 명시. 트리거 도달 시 처리 |
| RISK | 즉시 또는 단기 처리 권장. 사용자 컴펌 필요 |

---

## 작업 규칙

- 추측은 "추정" 또는 "검증 필요"로 명시. 단정과 구분.
- 모든 호출 시퀀스 화살표는 코드 출처(file:line) 첨부.
- 자체 검증 미통과 시 동일 흐름을 재검증 후 진행.
- 단계 종료 시 단계 종합 자체 검증 + 사용자 컴펌.
