# SUMMARY — DetectBase 2차 코드리뷰 통합 요약

> 14개 분석 문서를 1개로 압축. 자세한 내용은 각 문서 링크 참조.
> 이 문서만 읽어도 2차 리뷰 전체 결과를 파악 가능하도록 작성.

---

## §0. 한 줄 결론

**DetectBase 는 production-ready 베이스 프로젝트 수준이며, Phase A 9개 항목 (~50 LOC) 적용 후 첫 분기 프로젝트의 출발점으로 충분**.

---

## §1. 리뷰 범위 / 산출물

### 분석 흐름 9개 + 결합부 + 종합 = 11개 분석 문서

| ID | 흐름 | 문서 |
|---|---|---|
| F1 | Bootstrap & Shutdown Lifecycle | [F1_lifecycle.md](F1_lifecycle.md) |
| F2 | Configuration & Live Reload | [F2_configuration.md](F2_configuration.md) |
| F3 | Camera Pipeline (RTSP→NPU→Tracking→Abnormal) | [F3_pipeline.md](F3_pipeline.md) |
| F4 | Event Output (SocketIO/REST/IOWorker) | [F4_event_output.md](F4_event_output.md) |
| F5 | GRPC Bidirectional | [F5_grpc.md](F5_grpc.md) |
| F6 | Observability (Logger/Metrics/DiskGuard) | [F6_observability.md](F6_observability.md) |
| I1 | RTSP/HTTP/RTP Proxy & Server Core | [I1_rtsp_core.md](I1_rtsp_core.md) |
| I2 | RKNN API Headers | [I2_rknn_headers.md](I2_rknn_headers.md) |
| I3 | Generic BasicLibs | [I3_basiclibs.md](I3_basiclibs.md) |
| INTER_FLOW | 흐름 간 결합부 | [INTER_FLOW.md](INTER_FLOW.md) |
| FINDINGS | 등급별 정리 | [FINDINGS.md](FINDINGS.md) |

### ABCD 추가 분석

| ID | 산출물 |
|---|---|
| B (deferred 검증) | [DEFERRED_VERIFICATION.md](DEFERRED_VERIFICATION.md) |
| A (시스템 통합 뷰) | [SYSTEM_VIEW.md](SYSTEM_VIEW.md) |
| C (메타 리뷰) | [META_REVIEW.md](META_REVIEW.md) |
| D (이 문서) | SUMMARY.md |

---

## §2. 시스템 구조 (한 그림)

```
┌─────────────────────────────────────────────────────────────────────┐
│                    Service_DETECTOR (F1)                            │
│  ┌───────────────┐ ┌───────────────┐ ┌───────────────┐              │
│  │NetworkProfile │ │ServiceProfile │ │EngineProfiles │  (정적 F2)   │
│  └───────────────┘ └───────────────┘ └───────────────┘              │
│                                                                     │
│  ┌─────────────────────────────────────────────────────────────┐    │
│  │  NetworkManager (F4)                                        │    │
│  │  ├─ ApiHandler (REST)                                       │    │
│  │  ├─ SioHandler (SocketIO emit + listener)                   │    │
│  │  ├─ RtspHandler (I1)                                        │    │
│  │  └─ vector<GrpcEventClientBase> (F5, 조건부)                 │    │
│  └─────────────────────────────────────────────────────────────┘    │
│                                                                     │
│  ┌─────────────────────────────────────────────────────────────┐    │
│  │  IOStreamManager (F3) — 큐/디스패처 통합 관리               │    │
│  └─────────────────────────────────────────────────────────────┘    │
│                                                                     │
│  ┌─────────────────────────────────────────────────────────────┐    │
│  │  EngineLoadBalancer (F3)                                    │    │
│  │  ├─ engine_input_q (max=128)                                │    │
│  │  ├─ infer_respond_receiver                                  │    │
│  │  ├─ ReplyDispatcherWithCleaner<Output, UnitID>              │    │
│  │  └─ reply_dispatcher_thread                                 │    │
│  └─────────────────────────────────────────────────────────────┘    │
│                                                                     │
│  ┌──────────────────────┐       ┌──────────────────────────┐        │
│  │ engines_ (F3)        │       │ detector_block_ (F3)     │        │
│  │ NPU EngineHandler    │◄──────│  vector<RtspDetectorUnit>│        │
│  │ ├─ inference_thread  │       │  (per camera)            │        │
│  │ └─ rknn_run (~70ms)  │       │  ├─ inference_thread     │        │
│  └──────────────────────┘       │  ├─ io_worker_thread     │        │
│                                 │  ├─ avframe_q_           │        │
│                                 │  ├─ io_work_queue (30)   │        │
│                                 │  ├─ trackers_ (SORT)     │        │
│                                 │  ├─ scheduler_ (Abnormal)│        │
│                                 │  └─ SettingMonitor mixin │        │
│                                 └──────────────────────────┘        │
│                                                                     │
│  grpc_server_ (F5, 조건부)        SettingManager (F2 singleton)     │
│  └─ alive_handlers_ map           └─ 4 setter (Server/Cam/Excl/Sch) │
│                                                                     │
└─────────────────────────────────────────────────────────────────────┘
                  ▲                              ▼
          [Logger / Metrics (F6) — 모든 흐름이 사용]
```

---

## §3. 데이터 흐름 e2e (1 프레임)

```
[T+0ms] 카메라 RTSP RTP → I1 라이브러리 디코드 → AVFrame
[T+5ms] RtspDetectorUnit::avframe_q_ enqueue (RTSP thread → unit thread 경계)
[T+10ms] unit InferenceThread dequeue → 전처리 (sws_scale + blob)
[T+30ms] EngineLoadBalancer::RequestAsync → engine_input_q (max=128)
[T+50ms] NPU EngineHandler dequeue → rknn_run (~70ms 동기)
[T+125ms] rknn_outputs_get → YOLOv5 디코드 → respond_q
[T+125ms] reply_dispatcher_thread → set_reply(unit_id) → unit cv 깨움
[T+130ms] unit RespondAsync 깨어남 → 좌표 변환 → SORTTracker → AbnormalActions
[T+140ms] (이벤트 발생 시):
          ├─ SocketIO emit → emit_queue (max=1000) → emit_control_thread → 송신
          ├─ GRPC broadcast (조건부) → fire-and-forget
          └─ io_work_queue (max=30) → IOWorkerThread → cv::imwrite (~50-200ms)
                                                       └─ L1/L2-Regular/L2-Emergency
```

전체 latency 예산 ~75ms (DFPS 13.x). NPU sync run 이 70ms 차지.

자세한 내용은 [SYSTEM_VIEW.md §1](SYSTEM_VIEW.md#1-데이터-흐름-e2e--1-프레임의-일생) 참조.

---

## §4. 발견 사항 종합

### 4.1 등급별 통계

| 등급 | 기존 (Stage Synthesis) | DEFERRED 후 | META 후 (최종) |
|---|---|---|---|
| RISK | 0 | 0 | 0 |
| WARN | 14 | **16** (+ F-F5-07, F-F5-12) | 16 |
| NOTE | 38 | 33 (4건 INFO 강등) | **35** (+ M-04a, M-07a) |
| INFO | 27 | 31 (강등 4건) | **34** (+ M-08/09/10) |
| **합계** | **79** | **80** | **85** |

### 4.2 RISK 등급 (즉시 처리 필요)

**0건**. 보수적으로 NOTE/WARN 으로 분류됨.

### 4.3 WARN 16건 — 처리 권장도 분류

**A. 즉시 처리 권장 (Phase A — 약 50 LOC)**:

| ID | 작업 | LOC | 우선순위 |
|---|---|---|---|
| **F-F5-07** | GRPC client SetThreadFunctions 에 closer 추가 (cq_.Shutdown) | 1~3 | **높음 (dtor hang 차단)** |
| **F-F5-12** | BroadcastEventOnlyJsonToGrpcPeers 의 `catch(...)` 추가 | 3 | 중간 (noexcept 정합성) |
| F-F4-02 | emit_queue drop 메트릭 (errors_total{type="emit_drop"}) | 5~10 | 중간 (관측 가시성) |
| F-F3-01 | avframe_q_->SetMaxSize(2 * fps_limit) | 1 | 중간 (메모리 보호) |
| F-F1-04 / IF-05 | 종료 순서 코멘트 강화 (DETECTOR.cpp:376) | 3 | 낮음 (분기 작업자 보호) |
| F-F4-01 | GET_or_throw_if_timeout rename + try/catch 제거 | 10~20 | 낮음 (코드 정합성) |
| IF-02 | 다중 IOWorker emergency cleanup mtx try_to_lock | 5 | 낮음 (효율) |
| F-F6-04 | MAX_LOG_MSG_LEN 코멘트 수정 (2048→200KB) | 1 | XS (가독성) |
| F-F6-11 | README 메트릭 17→18 갱신 | 1 | XS (문서 정합성) |

**B. 검증 후 결정 (대부분 검증 완료 → 강등됨)**:
- W-01 (F-I3-01 dequeue throw) — 사용처 0건 → NOTE 강등 가능
- W-03 (F-F6-01 logger config) — F1 검증됨 → NOTE 강등 가능
- W-05 (F-F2-01 ApiHandler throw) — 더 이상 throw 안 함 → F-F4-01 로 이관

**C. 트리거 의존 (큰 작업)**:
- W-04 (F-F6-02 FileLogger throw 제거)
- W-06 (F-F2-02 nlohmann non-throwing API)
- W-07 (F-F1-01 Parser std::optional)
- W-13 (F-F5-01 server_owner_ weak_ptr)

**D. 변경 없음**:
- W-14 (F-F5-02 Client raw new — 외부 ABI 강제)
- W-02 (F-I3-02 SafeThread closer ASSERT — 가치 작음)

### 4.4 핵심 NOTE / INFO

**긍정 발견 (15건 INFO)**:
- F-F1-03 forward decl + .cpp dtor 패턴 (incomplete type 회피)
- F-F1-05 IgnoreSignalHandler async-signal-safe
- F-F2-05 RenewAfterReset 5-phase atomic 롤백
- F-F2-11 DefineDefault 단일 진실 (P39)
- F-F3-06 consecutive_mismatch_count_ 노이즈 보호
- F-F3-07 is_schedule_updated_ atomic flag (callback 짧게)
- F-F4-06 SioEventBinder mode flag 4개 (확장성)
- F-F5-04 Phase 2 fix 의 self-loopback 검증 통과
- F-F5-13 Phase 4 default post-processor
- F-F4-10 reconnect_listener 사용 (DEFERRED 검증)
- F-F3-15 InferenceThreadCloser 에서 자동 unsubscribe (DEFERRED 검증)
- ...

**META 단계 추가 발견**:
- M-04a GRPC InsecureChannelCredentials (production 분기 시 mTLS)
- M-07a 테스트 코드 부재 (분기 시 GoogleTest 권장)
- M-08 운영자용 OPERATIONS.md 부재
- M-09 정적 분석 도구 미적용
- M-10 emit_control_thread 정적 cid 부재

---

## §5. 결합부 (INTER_FLOW)

11개 결합부 모두 안전. 주요:

| Edge | Verdict | 핵심 |
|---|---|---|
| F2 ↔ F3 callback 락 외부 | 안전 | #10/#11 fix 적용 |
| F3 ↔ I1 avframe_q lifecycle | **안전** (DEFERRED 검증) | shared_ptr + InferenceThreadCloser 의 terminate + clear |
| F3 ↔ F4 emit/imwrite drop | 안전 | 백프레셔 동작. 메트릭 보강 권장 (Phase A) |
| F4 ↔ F5 outbound 독립 | 안전 | 의도된 fire-and-forget |
| F1 ↔ F3/F4/F5 종료 순서 | 안전 | 검증된 순서 + 코멘트 (강화 권장) |
| F6 ↔ 모든 흐름 cid | 안전 | thread별 정적 ID + JSON 본문 첨부 |

자세한 내용은 [INTER_FLOW.md](INTER_FLOW.md) 참조.

META 단계 추가 검증 (M-11/12/13) 모두 안전.

---

## §6. 시간축 시나리오 요약

### 부팅 (0~60s)
1. Logger init → Service ctor → Initialize (~10s) → Run → 정상 진입
2. 위험: SettingManager REST GET timeout / RTSP storm 방지 jitter

### 정상 운영
- 카메라당 2 thread (Inference + IOWorker) + 프로세스 7~10 thread
- DFPS 13.x. NPU sync run 이 bottleneck

### 종료
- SIGINT → atomic flag → Quit() 검증된 순서
- ⚠ **F-F5-07 미적용 시 GRPC client dtor hang 가능**

### 장애 (6 시나리오)
| 장애 | 자동 회복 |
|---|---|
| RTSP 끊김 | ✅ I1 재연결 |
| NPU 일시 실패 | ✅ 다음 frame 재시도 |
| 디스크 가득 | ✅ L2-Emergency |
| SocketIO 단절 | ✅ sioclient reconnect |
| GRPC peer down | ✅ channel reconnect |
| init SIGINT | 정책상 5회 무시 |

자세한 내용은 [SYSTEM_VIEW.md §2](SYSTEM_VIEW.md#2-시간축-시나리오) 참조.

---

## §7. 운영 모니터링

### 등록된 메트릭 18개 (Prometheus :9090/metrics)

```
detectbase_dfps_total                       (gauge)
detectbase_camera_count{state}              (gauge)
detectbase_events_total{type, cam}          (counter)
detectbase_errors_total{type}               (counter)
detectbase_socketio_reconnect_total         (counter)
detectbase_frame_disk_used_bytes            (gauge)
detectbase_frame_disk_capacity_bytes        (gauge)
detectbase_frame_disk_used_pct              (gauge)
detectbase_imwrite_skipped_total{reason}    (counter)
detectbase_frame_cleanup_deleted_total      (counter)
detectbase_frame_emergency_cleanup_total{type} (counter)
detectbase_grpc_client_enabled              (gauge)
detectbase_grpc_client_peer_count           (gauge)
detectbase_grpc_send_total{rpc}             (counter)
detectbase_grpc_server_enabled              (gauge)
detectbase_grpc_recv_total{rpc}             (counter)
detectbase_grpc_send_success_total{rpc}     (counter)
detectbase_grpc_send_failed_total{rpc, code}(counter)
```

### 사각지대 (Phase A 보강 대상)

| 부재 메트릭 | 영향 |
|---|---|
| engine_input_q drop | NPU bottleneck 인지 불가 |
| emit_queue drop | SocketIO 누적 인지 불가 |
| io_work_queue drop | 이미지 저장 누락 인지 불가 |
| setting callback 실패 | 설정 무반영 인지 불가 |
| logger 자체 실패 | 로그 누락 인지 불가 |

### 로그 진단 (correlation_id)

```bash
# 카메라별 추적
grep '"correlation_id":"sys-detector-658"' DetectBase.log

# IOWorker 추적
grep '"correlation_id":"sys-io_worker-' DetectBase.log

# SocketIO inbound 추적
grep '"correlation_id":"evt-' DetectBase.log
```

자세한 내용은 [SYSTEM_VIEW.md §3](SYSTEM_VIEW.md#3-운영자-관점--메트릭로그-만으로-진단) 참조.

---

## §8. 분기 프로젝트 (Master/Slave) 적용 가이드

### 모든 Phase 처리 완료 (2026-05-08, 32건 패치)

이전 "Phase A 9건 적용 권장" 항목은 모두 **이미 적용됨**. 추가로 자체 throw 11건 + 옵션 C 13건 + UpdateMode root cause fix + 옵션 1 NOTE 3건 + NOTE 4건 모두 적용 완료.

자세한 적용 항목은 [CODE_REVIEW_SUMMARY.md §6 - 2차 코드리뷰 후속 적용](../CODE_REVIEW_SUMMARY.md) 참조.

### 분기 프로젝트 변경점

- **NetworkSettings.json** — GRPC enabled true + peers
- **GrpcEventServerBase post-processor** — 실제 처리 로직 (Master 의 cross-check, Slave 의 카운터 송신)
- **Heartbeat thread** — 정기 동기화 thread 추가
- **도메인 클래스** — classes.yml + AbnormalActions 추가

### 분기 시 stress test 권장

- Master/Slave 양방향 1시간 부하 → F-F5-01 검증
- Slave kill/재시작 → reconnect (F-F5-08)
- Master/Slave 동시 종료 → F-F5-07 패치 검증
- 대용량 image RPC → max_message_size (F-F5-09)

자세한 내용은 [SYSTEM_VIEW.md §4](SYSTEM_VIEW.md#4-베이스-프로젝트-관점--fork-시-마주칠-수정점) 참조.

---

## §9. 분석 한계 (META 검증)

| 한계 | 설명 |
|---|---|
| L1 정적 분석만 | 코드 읽기로 추론. race / hang 발현은 검증 못 함 |
| L2 외부 라이브러리 black box | I1 / sioclient / grpc / opencv / ffmpeg 내부 미검토 |
| L3 분기 시뮬레이션 부재 | 분기 후 실제 거동 미검증 |
| L4 AI hallucination 가능성 | 메타 검증으로 일부 잡힘 (DEFERRED 단계) |

권장 보완:
- 분기 프로젝트가 unit test (GoogleTest) + integration test 도입
- clang-tidy / cppcheck 정적 분석 도구 적용
- Address/Thread Sanitizer 빌드로 race 탐지

자세한 내용은 [META_REVIEW.md](META_REVIEW.md) 참조.

---

## §10. 종합 평가

### 코드 품질 (2차 리뷰 후속 패치 모두 적용 후)

| 측면 | 평가 |
|---|---|
| RAII 적용 | ✅ 모든 리소스 (thread/queue/dispatcher/handler) RAII |
| 동시성 안전성 | ✅ 락 ordering 명확, callback 락 외부 호출 (#10/#11 fix) |
| Lifetime 관리 | ✅ shared/unique_ptr 체계적, Phase 2 fix 로 detached thread UAF 차단 |
| Backpressure | ✅ 모든 큐 max_size + drop oldest. **메트릭 5건 모두 추가됨** (drop / setting_callback / logger_fail) |
| 관측 가능성 | ✅ 18+ 메트릭 + JSON + correlation_id. **drop 메트릭 사각지대 모두 보강** |
| Disk Defense | ✅ 3-Layer (L1/L2-Regular/L2-Emergency/L3) 완비 + IF-02 try_to_lock |
| 종료 순서 | ✅ 검증된 순서. **F-F5-07 GRPC closer 패치 적용으로 hang 차단** |
| 에러 처리 | ✅ **자체 throw 0건** (CLAUDE.md "C++ 예외 사용 금지" 100% 준수). catch 는 외부 라이브러리 보호 차원만 |

### 가장 큰 발견

1. **UpdateMode bug (root cause)** — graceful degradation 으로 노출. 모든 카메라의 실제 schedule 정상 적용 회복.
2. **Phase 2 fix** (분석 6 GRPC UAF, 1차 리뷰) — `enable_shared_from_this` + handler registry. 2차 리뷰에서 정합성 검증.

### 베이스 프로젝트 적합성

✅ **production-ready baseline 확정**. 분기 프로젝트 시작 가능.

---

## §11. 잔존 작업

### 사용자 지시 시 처리 (보류)

- **Debug Virtual Lines 제거** — 시연/검증용 임시 코드. 사용자 명시 시 처리.

### 트리거 의존

- W-06 RenewAfterReset try/catch (nlohmann non-throwing 마이그레이션)
- W-13 server_owner_ raw ptr (Master/Slave stress)
- F-F5-08 / F-F5-09 (운영 관찰 / 대용량 RPC)
- F-I2-01 rknn_run async (성능)

### 분기 프로젝트 첫 task

- V-03 GoogleTest 인프라
- M-08 OPERATIONS.md 문서

### 변경 없음 / 사용자 기각

- 외부 ABI 강제: F-F5-02 raw new (gRPC), F-I1-02 new CRtspProxy (RTSP)
- 보안: V-01/V-02/V-04, M-04a (사용자 기각)
- 의도된 결정: P37 KST, F-F2-07 CameraSettingData

---

## §12. 다음 단계 (참고용 — 모든 Phase 처리 완료)

### 옵션 A — 종료

위 §11 의 잔존 작업은 모두 트리거 의존 또는 사용자 결정 항목. 즉시 처리 항목 없음.

### 옵션 B — 분기 프로젝트 시작

production-ready baseline 으로 fork.

- 정적 분석 도구 (clang-tidy / cppcheck) 적용
- unit test 인프라 구축 (GoogleTest)
- 운영 가이드 (OPERATIONS.md) 작성

---

**2차 코드리뷰 + 후속 패치 32건 모두 적용 완료**. production-ready baseline 확정.

> 2026-05-09 추가: 3차 코드리뷰 (자동화 도구 + 운영 시뮬레이션 + 차분 회귀) 진행 → 추가 결함 7건 fix. 자세한 내용 [REVIEW3/SUMMARY.md](../REVIEW3/SUMMARY.md).
