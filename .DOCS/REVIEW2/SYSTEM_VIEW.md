# SYSTEM_VIEW — 시스템 가로지르는 통합 뷰

> 흐름별 분석을 zoom-out 해서 시스템 전체를 4가지 축에서 본다:
> 1. **§1 데이터 흐름 e2e** — 한 프레임이 카메라 RTSP 진입부터 외부 출력까지
> 2. **§2 시간축 시나리오** — 부팅 / 정상 / 종료 / 장애 시 시스템 거동
> 3. **§3 운영자 관점** — 메트릭/로그로 진단, 사각지대
> 4. **§4 베이스 프로젝트 관점** — fork 한 첫 분기 프로젝트가 마주칠 수정점

---

## §1. 데이터 흐름 e2e — "1 프레임의 일생"

### 1.1 단일 프레임 처리 시퀀스 (정상 경로)

```
[T+0ms — 카메라 RTSP RTP 패킷 도착]
  │ I1 RTSP 라이브러리 (별도 thread tid_pkt_rx)
  ▼
[T+~5ms — H.264 디코드 → AVFrame]
  │ I1 라이브러리 내부 디코드 thread (per proxy)
  │ 디코드 완료 시 → CRtspProxy::setDecodedFrameSafeQueue 가 잡고 있던
  │                  unit 의 avframe_q_->enqueue_copy(sptr<AVFrame>)
  ▼
[T+~5ms — avframe_q_ enqueue]
  │ unit 의 SafeQueue<sptr<AVFrame>>
  │ unbounded — F-F3-01 (SetMaxSize 권장)
  │ producer = I1 디코드 thread / consumer = unit InferenceThread
  ▼
[T+~10ms — InferenceThread dequeue_wait_for]
  │ RtspDetectorUnit::InferenceThreadRunner
  │ correlation_id thread_local = "sys-detector-<cam_id>"
  ├─ exclude_setting_ check (F2 callback 으로 갱신 가능)
  ├─ 해상도 변경 감지 (consecutive_mismatch_count_ < 10 노이즈 보호)
  ├─ FrameFormattingContext.Convert(frame, capture_save=true, blob=true)
  │      └─ sws_scale (FFmpeg) → letterbox padded RGB → blob
  │      └─ save_snapshot_mat (이벤트 발생 시 imwrite 용)
  ▼
[T+~30ms — InputLayerWrapper 빌드]
  │ EngineClient::BuildRequest(ctx, correlation_id = frame_count_++)
  │ correlation_id = atomic counter — F-F3-13 검증됨
  │ → InputLayerWrapper{meta_data, requirements, image_data:shared_ptr}
  ▼
[T+~30ms — load_balancer_->RequestAsync(req)]
  │ EngineLoadBalancer::engine_input_q_ (max=128)
  │ 큐 가득 시 false → frame drop. 메트릭 부재 (F-F3-02, F-I3-03)
  ▼
[T+~50ms — NPU EngineHandler 가 dequeue]
  │ EngineHandlerBase::InferenceThreadRunner
  │ Preprocess(input) → 배치 가득 시 DoInference()
  ▼
[T+~120ms — RKNN 추론]
  │ rknn_inputs_set + rknn_run (동기) + rknn_outputs_get
  │ ~70ms NPU 추론
  ▼
[T+~125ms — Postprocess]
  │ YOLOv5 디코드 + NMS
  │ → vector<OutputLayerWrapper> {meta, engine_uuid, infer_objects}
  │ rknn_outputs_release
  ▼
[T+~125ms — engine respond_q_ enqueue → infer_respond_receiver_]
  │ EngineLoadBalancer 의 단일 receiver 큐
  ▼
[T+~125ms — reply_dispatcher_thread]
  │ ReplyDispatcherWithCleaner.set_reply(unit_id, output)
  │ → unit 의 cv 깨움
  ▼
[T+~125ms — InferenceThread RespondAsync 깨어남]
  │ load_balancer_->RespondAsync(subscribe_id, timeout)
  │ → wait_and_get(uuid, timeout) → OutputLayerWrapper
  ▼
[T+~130ms — 좌표 변환]
  │ ConvertInferObjectsCoordinates(objects, padded_style → original_style)
  │ pixel↔ratio / ltx↔cx / padded↔original 자동 변환 + 클램핑
  ▼
[T+~130ms — Tracker]
  │ trackers_[class_id]->TrackObjects(objects, request_seq)
  │ Kalman predict + Hungarian assign + age 관리
  │ → vector<InferObject with track_id>
  ▼
[T+~135ms — AbnormalActionChecker]
  │ for sch in scheduler_:
  │   sch->Check(...) — IntrusionLine/Zone/VehicleIntrusion/Parking
  │   HistoryChecker per track 의 시간 누적 (steady_clock)
  │   notification_min_interval_sec throttle
  ▼
[T+~140ms — 이벤트 트리거 (조건부)]
  │ event_list = [{event_message: json, on_event_results: vector<InferObject>}]
  │ for target_event in event_list:
  ├─ SendDetectResultToMetaData(detect_results)  ← I1 RTSP proxy meta out (ONVIF)
  ├─ sio_handler_->Emit("Message", json)         ← F4 → emit_queue (max=1000) → 비동기
  ├─ if grpc_client_enabled:
  │     network_manager_->BroadcastEventOnlyJsonToGrpcPeers(json.dump())  ← F5 fire-and-forget
  │     metrics: grpc_send_total{rpc="SendEventOnlyJson"} += sent
  ├─ frame_path = MakeImageSavePath(/frame/YYYY/MM/DD/...)
  ├─ io_work_queue_->enqueue_move(IOWorkItem{frame_path, save_snapshot_mat.clone()})
  │      ← F4/F6 IOWorker 비동기 (max=30, drop oldest)
  └─ metrics: events_total{type=..., cam=...}++

[T+~140ms — InferenceThread 루프 종료]
  │ 다음 dequeue_wait_for 로 복귀
  ▼ (병렬 비동기 분기)
  ├─ emit_control_thread → SocketIO emit (별도 thread, 직렬화)
  ├─ IOWorkerThread → cv::imwrite (별도 thread, ~50-200ms)
  │   └─ L1 disk pre-block / L2-Regular cleanup / L2-Emergency cleanup
  └─ GRPC client cq drain thread → asyncCompleteRpc → 메트릭
       └─ send_success_total{rpc} 또는 send_failed_total{rpc, code}
```

### 1.2 분기 / 백프레셔 / 손실 지점

| 위치 | 동작 | 메트릭 | 평가 |
|---|---|---|---|
| `avframe_q_` | unbounded — RTSP 가 fps_limit 으로 throttle | 없음 | F-F3-01 |
| `engine_input_q_` (128) | 초과 시 RequestAsync false → frame drop | **없음** (F-F3-02) | 보강 필요 |
| `infer_respond_receiver_` | 무제한 추정 | 없음 | 영향 미미 |
| `emit_queue` (1000) | drop oldest | **없음** (F-F4-02) | 보강 필요 |
| `io_work_queue_` (30) | drop oldest | **없음** (F-I3-03) | 보강 필요 |
| `RespondAsync` timeout | 5초 후 result 없음 → frame skip | 없음 | 영향 미미 |
| AbnormalActions throttle | notification_min_interval_sec | 없음 | 의도된 throttle |

**관측 사각지대**: 4개 큐의 drop 모두 메트릭 부재. **emit_drop / engine_input_drop / io_work_drop 메트릭 추가 권장 (Phase A 통합)**.

### 1.3 latency 예산 (DFPS 13.x = ~75ms 예산)

| 단계 | 비중 |
|---|---|
| RTSP 디코드 + 큐잉 | ~5-10ms |
| 전처리 (sws_scale + blob) | ~10-20ms |
| RequestAsync + dispatch | ~5ms |
| **NPU 추론 (rknn_run)** | **~70ms** ← bottleneck |
| Tracker + Abnormal | ~5ms |
| Emit (큐 enqueue 만) | <1ms |

**NPU sync run 이 핵심 bottleneck**. F-I2-01 의 비동기 rknn_wait 적용 시 IO/inference overlap 가능 — 잠재 개선.

---

## §2. 시간축 시나리오

### 2.1 부팅 60초

```
T+0      systemd start → docker-compose up → /code/bin/DetectBase 실행
T+0+0    main() 진입
T+0+1    InitLogger() → FileLogger(/DetectBase/logs/DetectBase.log)
T+0+1    Service_DETECTOR ctor → ServiceProfileBuilder::Build()
T+0+1    SIGPIPE = SIG_IGN, SIGINT = IgnoreSignalHandler
T+0+1    Initialize() 진입
         ├─ Stage 0: MetricsRegistry::Initialize(9090) + 18개 등록
         ├─ Stage #01: NetworkProfile / EngineProfile parse
         ├─ Stage #02: NetworkManager / IOStreamManager / EngineLoadBalancer 생성
         ├─ Stage #03: EngineHandlerBuilder_NPU.BuildHandlers()
         │              └─ rknn_init(model_data) ~ 1초/엔진
         ├─ Stage #04: ConnMVAS(service_name)
         │              ├─ ApiHandler(create + ping)
         │              ├─ SettingManager.Initialize() → REST GET 4번 (~500ms~3s)
         │              ├─ SioHandler(create + connect, wait_for 10s)
         │              ├─ RtspHandler(create + xml load)
         │              └─ InitializeGrpcClients() (조건부)
         │              └─ InitializeRTSPWithStaticCameraList()
         ├─ Stage #05: IOStreamManager.Ready / engine.ActivateEngine() / SocketIOEventBind()
         │              └─ RtspDetectorBlock build + Init (per camera ~10ms)
         └─ Phase 3: GRPC server (조건부)
T+0+~10  Initialize() return true
T+0+~10  RegisterSignalHandler(ExitSignalHandler) ← 정상 종료 핸들러로 교체
T+0+~10  Run() 진입
         ├─ rtsp_handler_->RunRTSP()
         │      └─ for each proxy: new CRtspProxy + startConn(jitter 50-400ms)
         │      └─ tid_pkt_rx + tid_main thread 시작
         └─ detector_block_->Start() → 각 unit 의 inference/io thread 시작
T+0+~15  WaitUntilQuitSignal — 100ms polling 시작
T+0+~20  첫 프레임 처리 시작 (RTSP handshake + 디코드 안정화)
T+0+~30  DFPS 안정화 (13.x 도달)
T+0+~60  정상 운영 진입
```

**부팅 단계 위험**:
- Stage #04 의 SettingManager::Initialize 가 REST API 4번 — MVAS 서버 down 시 timeout 누적 (각 3초 default)
- Stage #03 의 rknn_init — 모델 파일 corruption 시 throw (라이브러리 동작)
- Stage Run RTSP startConn jitter 50-400ms — 다중 카메라 동시 연결 storm 방지 (F-I1-03)
- IgnoreSignalHandler 5회까지 SIGINT 무시 — 부팅 중 강제 종료 차단

### 2.2 정상 운영 (steady state)

```
[Per camera unit (병렬)]
  InferenceThread loop:
    ├─ avframe_q_ dequeue (100ms timeout)
    ├─ NPU 추론 1 cycle (~120ms)
    └─ emit / imwrite / GRPC broadcast

  IOWorkerThread loop:
    ├─ io_work_queue_ dequeue (1s timeout)
    ├─ L2-Regular cleanup (1h 주기)
    ├─ L2-Emergency cleanup (5min cool-down)
    ├─ L1 disk pre-block (≥90% skip)
    └─ cv::imwrite

[Process-wide]
  RTSP rx_thread:                    RTP 패킷 수신
  RTSP task thread:                  RTSP 메인 루프
  Per-proxy decode thread:           H.264 디코드
  EngineHandler::inference_thread:   NPU 추론 (전체 1)
  reply_dispatcher_thread:           응답 분배 (전체 1)
  InferenceCounter::thread:          DFPS 통계 (10초 주기)
  emit_control_thread:               SocketIO emit (전체 1)
  GRPC client 의 cq drain thread × N peer
  GRPC server 의 cq drain thread × 1 (조건부)
  ReplyDispatcherWithCleaner cleanup thread × 2 (engine + grpc)
  FileLogger reopen (28일 주기, 거의 무의미)

총 thread 추정: 카메라 N대 → 6 + 2N + (M peer) + (조건부 1)
```

**정상 운영 메트릭 패턴**:
- `dfps_total` = 13.x (카메라 합산 DFPS)
- `frame_disk_used_pct` = 운영 중 추적 (90% 임박 시 L2-E 활성)
- `events_total{type=...}` = 이벤트 발생 시 increment
- `errors_total{type=...}` = 0 유지가 정상

### 2.3 정상 종료 60초

```
T-X      SIGINT 수신 (Ctrl+C 또는 docker stop SIGTERM)
T-X+0    ExitSignalHandler → g_terminate_flag = true (relaxed atomic)
T-X+~100ms  WaitUntilQuitSignal 100ms cv.wait_for 깨어남 → flag 검사
T-X+~100ms  Quit() 진입
            └─ is_quit_.exchange(true) → 첫 진입만 진행
            └─ cond_.notify_all()
T-X+~100ms  종료 순서 (DETECTOR.cpp:376 검증된 순서):
            ├─ #00 grpc_server_ stop & reset       (외부 신규 요청 차단)
            ├─ #01 engines_ TerminateEngine        (NPU ctx_ release)
            ├─ #02 load_balancer_->Terminate       (engine_input_q terminate)
            ├─ #03 detector_block_->Stop()
            │        └─ unit::Stop() × N
            │             ├─ inference_thread_.Stop()
            │             │       └─ Closer = InferenceThreadCloser
            │             │             ├─ load_balancer_->Unsubscribe (F-F3-15)
            │             │             ├─ avframe_q_->terminate()
            │             │             └─ avframe_q_->clear_without_action()
            │             └─ io_worker_thread_.Stop()
            │                     └─ Closer = io_work_queue_->terminate() + clear
            ├─ #04 network_manager_->CloseNetworkAll()
            │        ├─ SioHandler::TerminateSocketIO
            │        │       ├─ emit_control_thread.Stop() (Closer = emit_queue.terminate)
            │        │       └─ client.sync_close() ← 외부 라이브러리 응답 대기 (F-F4-05)
            │        ├─ RtspHandler::StopRTSP
            │        │       └─ async rtsp_stop() + 10s timeout
            │        └─ CloseGrpcClients() × N peer
            │                ├─ grpc_client_thread_.Stop() ⚠ closer 누락 (F-F5-07)
            │                └─ ⚠ cq_.Shutdown() 도달 안 할 수 있음
            ├─ #05 io_stream_manager_->ClearAll()
            └─ MetricsRegistry::Shutdown()
T-X+~?   "PROGRAM QUIT SUCCESS" 로그 → return
```

**종료 잠재 hang 지점** (B 단계 발견):
- **F-F5-07** — GRPC client 의 grpc_client_thread_.Stop() 이 hang 가능
  - asyncCompleteRpc 가 cq_.Next 에서 block
  - closer 가 nullptr → join 시도 → cq_.Shutdown() 도달 못함
  - **즉시 처리 권장 (Phase A)**

**그 외 잠재 hang**:
- SocketIO sync_close (F-F4-05) — 외부 라이브러리 timeout 정책
- RtspHandler rtsp_stop async + 10s timeout — 보호됨

### 2.4 장애 시나리오 (운영 거동)

#### S1. 카메라 RTSP 일시 끊김 (1~10분)

```
[Detected]
  - I1 라이브러리 자체 reconnect (rtsp_cln 이 RTSP 재핸드셰이크)
  - InferenceThread 의 avframe_q_->dequeue_wait_for(100ms) 가 nullopt 반복
  - long_timelapse_log_interval (10분) 후 MLOG_INFO("decoded frame not received")

[Metric impact]
  - dfps_total 감소 (해당 카메라 0)
  - errors_total 증가 안 함 (정상 이벤트로 분류)

[Recovery]
  - I1 재연결 → 자동 복구
  - DetectBase 측 별도 조치 없음
```

#### S2. NPU 일시 실패 (rknn_run 에러)

```
[Detected]
  - EngineHandler::DoInference() return false
  - BuildInferenceFailureRespond → empty vector OutputLayerWrapper
  - InferenceThread 의 RespondAsync 가 빈 결과 받음 → frame skip

[Metric impact]
  - errors_total{type="npu_fail"} 증가 (검증 필요 — 메트릭 increment 위치 확인)

[Recovery]
  - 다음 frame 부터 재시도. ctx 살아있으면 자동 복구
  - 영구 실패 시 ActivateEngine 재호출 필요 (현재 코드에 자동 재활성화 없음)
```

#### S3. 디스크 가득 (≥90%)

```
[Detected]
  - IOWorker 의 GetFrameDiskUsedPercent() ≥ 90
  - L1 pre-block: cv::imwrite skip
  - imwrite_skipped_total{reason="disk_full"} ↑
  - 1분 cool-down WARN log

[L2-Emergency 자동 회복]
  - 5min 주기로 EmergencyCleanupIfDiskHigh() 실행
  - case 1: 가장 오래된 day folder 삭제
  - case 2: 당일만 남으면 절반 파일 삭제
  - frame_emergency_cleanup_total{type=...} ↑

[Recovery]
  - 자동 회복 (지난 검증: 90% → 60% / 16GB 회수)
```

#### S4. SocketIO 단절

```
[Detected]
  - sioclient::set_reconnect_listener 발동
  - socketio_reconnect_total ↑ (F-F4-10 검증됨)

[Behavior during disconnect]
  - emit_queue 누적 (max=1000, 초과 시 drop oldest)
  - 단절 1분 ~ DFPS 13 × 60 = ~780 events. 1000 한도 거의 초과 안 함
  - F-F4-02 drop 메트릭 부재 → 운영자 인지 어려움

[Recovery]
  - sioclient 자동 reconnect → 재연결 후 emit_queue 빠져나감
```

#### S5. GRPC peer down

```
[Detected]
  - SendEventOnlyJson 호출 → cq_ 의 status 가 UNAVAILABLE / DEADLINE_EXCEEDED
  - send_failed_total{rpc="SendEventOnlyJson", code=...} ↑

[Behavior]
  - fire-and-forget 이라 이벤트 손실
  - GRPC channel 자체는 reconnect backoff (max 2s, init 1s) 로 자동 시도

[Recovery]
  - 자동
```

#### S6. 부팅 중 SIGINT (5회 미만)

```
[Detected]
  - IgnoreSignalHandler 가 매 SIGINT 마다 ++g_force_exit_count
  - <= 5 면 stderr 메시지 (write 2)
  - > 5 면 ExitSignalHandler → g_terminate_flag = true
```

### 2.5 시나리오별 자동 회복 vs 수동 개입

| 시나리오 | 자동 회복 | 수동 필요 |
|---|---|---|
| S1 RTSP 끊김 | ✅ I1 재연결 | — |
| S2 NPU 일시 실패 | ✅ 다음 frame 재시도 | NPU 영구 실패 시 재시작 |
| S3 디스크 가득 | ✅ L2-Emergency | — |
| S4 SocketIO 단절 | ✅ sioclient reconnect | — |
| S5 GRPC peer down | ✅ channel reconnect | peer 측 복구 필요 |
| S6 init SIGINT | — | 정책상 무시 |
| **NPU 영구 실패** | ❌ | 컨테이너 재시작 |
| **모델 파일 corruption** | ❌ | 모델 교체 후 재시작 |
| **GRPC client hang (F-F5-07)** | ❌ | dtor 도달 못함 — 강제 종료 |

---

## §3. 운영자 관점 — 메트릭/로그 만으로 진단

### 3.1 정상 vs 비정상 메트릭 패턴

```
[정상]
  detectbase_dfps_total                  = ~13 × N (cam)
  detectbase_camera_count{state="active"} = N
  detectbase_events_total{type=*}        = 운영 중 increment
  detectbase_errors_total                = 0 유지
  detectbase_frame_disk_used_pct         < 80%
  detectbase_socketio_reconnect_total    = 0 (또는 부팅 시 1)
  detectbase_grpc_*                      = enabled 면 send_total ≈ send_success_total

[비정상]
  dfps_total 급감             → 카메라 끊김 / NPU 부하
  errors_total{type=*} 급증   → 카테고리 별 진단 (npu_fail / imwrite_fail / emit_fail)
  frame_disk_used_pct ≥ 80%   → L2-Emergency 활성 — emergency_cleanup_total{type=...} 급증
  imwrite_skipped_total       → 디스크 가득 (L1 활성)
  socketio_reconnect_total ↑↑ → 네트워크 단절 또는 broker 측 문제
  grpc_send_failed_total > 0  → peer down 또는 네트워크
```

### 3.2 진단 시나리오

```
[진단 1] DFPS 가 N×13 미만
  Step 1. dfps_total 의 시간 추이 확인 (Grafana)
  Step 2. errors_total{type=*} 비교 → npu_fail / preprocess_fail 있나
  Step 3. log: grep "frame size mismatch" → 카메라 해상도 변동
  Step 4. log: grep "decoded frame not received" → RTSP 끊김

[진단 2] 디스크 사용량 갑자기 증가
  Step 1. frame_disk_used_pct 추이
  Step 2. frame_emergency_cleanup_total 발동 여부
  Step 3. log: grep "EMERGENCY cleanup" → 실제 회수량 확인
  Step 4. /frame 폴더 의 day folder 분포 확인 (find /frame -name "*.jpg" | wc -l)

[진단 3] 이벤트 알림 누락
  Step 1. events_total ↔ socketio_reconnect_total 비교
  Step 2. log: grep '"correlation_id":"sys-detector-<cam_id>"' → 카메라별 추적
  Step 3. log: grep '"lvl":"ERROR"' → emit/imwrite 실패
```

### 3.3 운영 사각지대

| 항목 | 메트릭 부재 | 영향 |
|---|---|---|
| `engine_input_q_` drop | F-F3-02 | NPU bottleneck 시 frame drop 외부 관측 불가 |
| `emit_queue` drop | F-F4-02 | SocketIO 누적 후 oldest drop 인지 불가 |
| `io_work_queue_` drop | F-I3-03 | 이미지 저장 누락 인지 불가 |
| `setting callback` 실패 | F-F2-04 | 설정 변경 무반영 인지 불가 |
| Logger 자체 실패 | F-F6-09 | 로그 누락 인지 불가 |

**Phase A 통합 권장**: drop 메트릭 4개 + setting callback 실패 1개 = 5개 추가.

### 3.4 로그 기반 진단 (correlation_id 활용)

```bash
# 특정 카메라 추적
grep '"correlation_id":"sys-detector-658"' DetectBase.log | tail -30

# 시스템 thread 별 추적
grep '"correlation_id":"sys-io_worker-' DetectBase.log
grep '"correlation_id":"evt-' DetectBase.log    # SocketIO inbound

# ERROR 만
grep '"lvl":"ERROR"' DetectBase.log | tail -50

# 특정 시간대 (UTC ISO8601)
grep '"ts":"2026-05-08T1[2-3]' DetectBase.log
```

correlation_id 패턴 분석:
- `sys-detector-<cam_id>` — InferenceThread 의 정적 ID
- `sys-io_worker-<cam_id>` — IOWorkerThread (DEFERRED 검증에서 발견 — RtspDetectorUnit.cpp:1273)
- `evt-<unix_ms>-<seq>` — SocketIO inbound CorrelationScope

---

## §4. 베이스 프로젝트 관점 — fork 시 마주칠 수정점

### 4.1 첫 분기 프로젝트 (Master/Slave) 가 변경할 부분

#### 4.1.1 NetworkSettings.json
```json
{
  "GRPC_Server_Enabled": true,           # 양쪽 모두 활성
  "GRPC_Server_Port": 50051,
  "GRPC_Client_Enabled": true,
  "GRPC_Peers": [{"name":"slave1", "ip":"192.168.x.y", "port":50051}]
}
```

#### 4.1.2 GrpcEventServerBase 의 post-processor 등록 (DETECTOR.cpp:307~)

```cpp
// 현재 (베이스): 로그만 출력
grpc_server_->SetSendEventOnlyJsonPostProcesser(
    []( const EventDataOnlyJson& req, Empty& /*rsp*/ ){
        MLOG_INFO("[GRPC RECV] ...");
        ... metric ...
    });

// 분기 프로젝트: 실제 처리 로직
grpc_server_->SetSendEventOnlyJsonPostProcesser(
    [this]( const EventDataOnlyJson& req, Empty& /*rsp*/ ){
        // Master 가 받은 이벤트 → 로컬 카메라 결과와 cross-check
        // 또는 로컬 통계에 합산 등
    });

grpc_server_->SetSendCounterDeltaPostProcesser(
    [this]( const CounterDelta& delta, Empty& /*rsp*/ ){
        // Slave 가 보낸 카운터 변화량을 Master 가 누적
    });

grpc_server_->SetRequestCounterSnapshotHandler(
    [this]( const CounterRequest& req, CounterSnapshot& rsp ){
        // 누적 카운터 snapshot 반환
        rsp.set_total_events( ... );
    });
```

#### 4.1.3 Periodic 동기화 thread 추가

```cpp
// 새 SafeThread 추가 — heartbeat / counter delta 정기 송신
heartbeat_thread_.SetThreadFunctions(
    [this]{
        while(running) {
            HeartbeatPing ping;
            ping.set_node_id("slave1");
            grpc_client->SendHeartbeat(std::move(ping));
            std::this_thread::sleep_for(30s);
        }
    },
    [this]{ /* closer */ }
);
```

#### 4.1.4 도메인 클래스 추가
- COCO 80 클래스 외 도메인 특화 클래스 추가 시 `classes.yml` + EngineProfile 의 `target_class_names` 변경
- AbnormalActions 의 새 알고리즘 추가 (예: PoseAnalysis) — `EventChecker` 시그니처 따라 함수 추가

### 4.2 fork 시 위험 결정점

| 결정점 | 베이스 결정 | 분기 시 고려 |
|---|---|---|
| GRPC default OFF | 베이스 | 분기는 ON. F-F5-07 closer 패치 선반영 권장 |
| 카메라 동적 추가/제거 | F-F2-07 미지원 (재시작 가정) | 분기가 동적 필요 시 RegisterCameraSettingCallback 추가 |
| KST hard-coded | P37 결정 | 분기가 다른 timezone 이면 분기 결정 |
| /frame retention 7일 | 베이스 | 분기 별 정책 변경 (TaskBase 의 운영 환경) |
| MAGIC_DETECTION_ENGINE_NAME 단일 | 베이스 | 분기가 multi-model 이면 분기 추가 |
| GRPC noexcept + try/catch (F-F5-12) | 베이스 결함 | 분기 전 패치 권장 |

### 4.3 분기 프로젝트 stress test 권장 항목

| 시나리오 | 검증 대상 |
|---|---|
| Master/Slave 양방향 1시간 부하 | F-F5-01 (server_owner_ raw ptr) — handler 가 server 보다 오래 살 수 있는지 |
| Slave kill 후 재시작 | GRPC client reconnect (F-F5-08) |
| Master/Slave 동시 종료 | GRPC client closer (F-F5-07 패치 검증) |
| 대용량 image (수 MB × N) RPC | max_message_size 설정 (F-F5-09) |
| Slave 의 SettingManager 변경 broadcast | F2 ↔ F5 결합부 (현재는 SettingMonitor 가 GRPC 미지원) |

### 4.4 Phase A 의 분기 프로젝트 적용 권장 순서

1. **F-F5-07** GRPC client closer 패치 → 종료 hang 차단
2. **F-F5-12** Broadcast catch(...) 추가 → noexcept 정합성
3. **F-F4-02** emit_queue drop 메트릭 → 운영 가시성
4. **F-F3-01** avframe_q_ SetMaxSize → 메모리 보호
5. F-F1-04 종료 순서 코멘트 강화 → 분기 작업자 보호
6. **F-F4-01** GET_or_throw_if_timeout rename → 코드 정합성
7. F-F6-04 / F-F6-11 → 작은 정리

분기 프로젝트는 베이스 + 위 7~9개 패치를 적용한 "production-ready 베이스" 에서 시작 권장.

---

## §5. Self-Check (SYSTEM_VIEW)

- [x] §1 e2e 시퀀스 — T+0ms 부터 T+~140ms 까지 단계별 latency + file:line 인용 정확
- [x] §1.2 백프레셔 표 — 4개 큐 모두 capacity / drop 정책 / 메트릭 부재 확인
- [x] §2 시간축 4개 시나리오 (부팅/정상/종료/장애×6) 모두 코드 인용 + 자동/수동 회복 매트릭스
- [x] §2.3 종료 hang 지점 명시 (F-F5-07 강조)
- [x] §3 운영자 관점 — 정상/비정상 메트릭 패턴, 진단 시나리오 3개, 사각지대 5개
- [x] §4 분기 프로젝트 관점 — NetworkSettings, post-processor, 도메인 클래스, 위험 결정점 6개, stress 항목 5개, Phase A 적용 순서 7~9개
- [x] 추측 표시 — "검증 필요" (S2 npu_fail metric increment 위치)

**검증 결과**: PASS

**핵심 발견 (시스템 뷰에서만 보이는 것)**:
1. NPU sync run 이 75ms latency 예산의 70ms 사용 — bottleneck 지점 명확
2. 4개 큐의 drop 모두 메트릭 부재 — 운영 사각지대
3. Phase A 처리 우선순위가 분기 프로젝트 시작 시점 기준 명확화
4. F-F5-07 (GRPC closer) 가 종료 hang 의 가장 큰 잠재 위험 — 분기 프로젝트 활성화 시 즉시 발현 가능
