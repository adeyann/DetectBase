# F3 — Camera Pipeline (RTSP → NPU → Tracking → Abnormal → Event)

## §1. Why

DetectBase 의 메인 흐름. 카메라 RTSP 영상을 받아 객체 검출 → 추적 → 이상행동 검출까지의 전 과정. 가장 무거운 흐름.

핵심 단계:
1. **RTSP 입력**: CRtspProxy 가 카메라 RTSP 디코딩 → AVFrame 큐로 push
2. **전처리**: AVFrame → cv::Mat → letterbox resize → blob (RGB uint8 buffer)
3. **NPU 추론**: LoadBalancer 의 단일 큐로 전달 → RKNN 추론 → InferObject[] 반환
4. **트래킹**: SORTTracker (Kalman + Hungarian) → track_id 부여
5. **이상행동**: AbnormalActionChecker (LineIntrusion / AreaIntrusion / VehicleIntrusion / VehicleParking)
6. **이벤트 출력**: SioHandler emit + RTSP proxy meta + IOWorker 의 cv::imwrite (비동기)

제거 시 잃는 것: 프로젝트 자체. 다른 모든 흐름은 이 흐름을 위해 존재.

---

## §2. Roster

### Primary (F3)

| 카테고리 | 파일 |
|---|---|
| **Detector Block/Unit (메인 루프)** | [Main/DETECTOR/include/RtspDetectorBlock.h](../../code/Main/DETECTOR/include/RtspDetectorBlock.h), [Main/DETECTOR/src/RtspDetectorBlock.cpp](../../code/Main/DETECTOR/src/RtspDetectorBlock.cpp), [Main/DETECTOR/include/RtspDetectorUnit.h](../../code/Main/DETECTOR/include/RtspDetectorUnit.h), [Main/DETECTOR/src/RtspDetectorUnit.cpp](../../code/Main/DETECTOR/src/RtspDetectorUnit.cpp) (~1370 라인, F3 의 중심) |
| **RTSP 카메라 핸들러** | [worker/include/RtspHandler.h](../../code/Management/worker/include/RtspHandler.h), [worker/src/RtspHandler.cpp](../../code/Management/worker/src/RtspHandler.cpp) |
| **Vision 전처리** | [VisionCommon/include/FramePreProcessor.h](../../code/VisionCommon/include/FramePreProcessor.h), [VisionCommon/src/FramePreProcessor.cpp](../../code/VisionCommon/src/FramePreProcessor.cpp), [VisionCommon/include/SwsContextManager.h](../../code/VisionCommon/include/SwsContextManager.h), [VisionCommon/src/SwsContextManager.cpp](../../code/VisionCommon/src/SwsContextManager.cpp) |
| **NPU 엔진** | [Engine/EngineBase/include/EngineHandlerBase.h](../../code/Engine/EngineBase/include/EngineHandlerBase.h), [Engine/EngineBase/src/EngineHandlerBase.cpp](../../code/Engine/EngineBase/src/EngineHandlerBase.cpp), [Engine/NPU/EngineBuilder/EngineHandlerBuilder_NPU.{h,cpp}](../../code/Engine/NPU/EngineBuilder/), [Engine/NPU/YoloV5_Torch_Onnx_RKNN_NPU/YoloV5_Torch_Onnx_RKNN_NPU.{h,cpp}](../../code/Engine/NPU/YoloV5_Torch_Onnx_RKNN_NPU/) |
| **Engine 분배기** | [manager/include/EngineLoadBalancer.h](../../code/Management/manager/include/EngineLoadBalancer.h), [manager/src/EngineLoadBalancer.cpp](../../code/Management/manager/src/EngineLoadBalancer.cpp), [manager/include/IOStreamManager.h](../../code/Management/manager/include/IOStreamManager.h), [manager/src/IOStreamManager.cpp](../../code/Management/manager/src/IOStreamManager.cpp), [worker/include/EngineClient.h](../../code/Management/worker/include/EngineClient.h), [worker/src/EngineClient.cpp](../../code/Management/worker/src/EngineClient.cpp), [worker/include/InferenceCounter.h](../../code/Management/worker/include/InferenceCounter.h), [worker/src/InferenceCounter.cpp](../../code/Management/worker/src/InferenceCounter.cpp) |
| **Tracker (SORT)** | [Tracker/SORT/SORTTracker.{h,cpp}](../../code/Tracker/SORT/), [Tracker/SORT/MgenKalman.{h,cpp}](../../code/Tracker/SORT/), [Tracker/SORT/MgenHungarian.{h,cpp}](../../code/Tracker/SORT/), [Tracker/TrackerBase/TrackerTypes.h](../../code/Tracker/TrackerBase/TrackerTypes.h) |
| **Abnormal Action** | [AbnormalActions/include/AbnormalActionChecker.h](../../code/AbnormalActions/include/AbnormalActionChecker.h), [AbnormalActions/src/AbnormalActionChecker.cpp](../../code/AbnormalActions/src/), [AbnormalActions/include/HistoryChecker.h](../../code/AbnormalActions/include/HistoryChecker.h), [AbnormalActions/include/MgenSchedule.h](../../code/AbnormalActions/include/MgenSchedule.h), [AbnormalActions/src/MgenSchedule.cpp](../../code/AbnormalActions/src/MgenSchedule.cpp), [AbnormalActions/include/GeometricLogic.h](../../code/AbnormalActions/include/GeometricLogic.h), [AbnormalActions/src/GeometricLogic.cpp](../../code/AbnormalActions/src/GeometricLogic.cpp), [AbnormalActions/include/ScheduleTypes.h](../../code/AbnormalActions/include/ScheduleTypes.h) |

### Also-touches

| 흐름 | 활용 |
|---|---|
| I1 | CRtspProxy / ProxyVideoInfo / setDecodedFrameSafeQueue |
| I2 | RKNN API 8개 함수 (rknn_init/destroy/query/inputs_set/run/outputs_get/release) |
| I3 | SafeQueue/SafeThread/InferObject/EngineStreamTypes/ClassChecker/file_utils 광범위 |
| F2 | SettingMonitor 상속 + SubscribeSetting<Schedule/Exclude>, SettingManager singleton |
| F4 | NetworkManager / SioHandler / RtspHandler / IOWorker (cv::imwrite) → Event Output |
| F5 | NetworkManager 의 GRPC client broadcast 지점 (이벤트 emit 지점에서) |
| F6 | CorrelationScope (`sys-detector-<id>`) + 메트릭 광범위 (events/errors/dfps/imwrite_skipped/cleanup) + Disk Defense (RtspDetectorUnit.cpp 내 임베디드) |

---

## §3. How

### 3.1 메인 호출 시퀀스 (F1 → F3)

```
[F1 Stage #05]
  detector_block_ = make_unique<RtspDetectorBlock>(profile, network, io, balancer)
  detector_block_->BuildServiceUnit(10ms)
    └─ for cam_id in SettingManager.GetCameraIDSet():
         service_units_.push_back(make_unique<RtspDetectorUnit>(cam_id, ...))
         sleep(10ms)        ← 동시 생성 burst 분산
  detector_block_->Init(30ms)
    └─ for unit in service_units_: unit->Init()
         └─ EngineLoadBalancer ptr / NetworkManager / RtspHandler / SioHandler 검증
         └─ avframe_q_ = make_shared<SafeQueue<sptr<AVFrame>>>()
         └─ inference_thread_.SetThreadFunctions(Runner, Closer)
         └─ io_work_queue_ 생성 (max_size=30)
         └─ io_worker_thread_.SetThreadFunctions(IOWorkerRunner, IOWorkerCloser)
         └─ SubscribeSetting<ExcludeCamSettingData>(callback, cam_id)
         └─ SubscribeSetting<ScheduleSettingData>(callback, cam_id)

[F1 Stage Run]
  rtsp_handler_->RunRTSP()                   ← I1, RTSP 라이브러리 thread 시작
  detector_block_->Start(30ms)
    └─ for unit in service_units_: unit->Start()
         └─ inference_thread_.Start()        ← InferenceThreadRunner thread 시작
         └─ io_worker_thread_.Start()        ← IOWorkerThreadRunner thread 시작
```

### 3.2 InferenceThreadRunner 메인 루프 (per camera)

```
InferenceThreadRunner (RtspDetectorUnit.cpp:698~):
  [Setup]
    CorrelationContext::Set("sys-detector-<cam_id>")        ← P53
    EngineClient client.Init(magic="DetectionEngine", classes={"Person","Car"})
    detection_engines.push_back(client)
    load_balancer_->Subscribe(subscribe_id)

  [Warm-up]
    proxy_ptr_ = rtsp_handler_->GetProxyPtr(id_)            ← retry 100ms
    proxy_ptr_->setDecodedFrameSafeQueue(avframe_q_, true, detect_fps_limit_)
                                                            ← I1 RTSP 가 frame push

  [Main Loop while running]
    1. avframe_q_->dequeue_wait_for(100ms)                 ← I3 SafeQueue
    2. exclude_setting_ check                              ← F2 callback 으로 갱신됨
    3. 해상도 변경 감지 (consecutive_mismatch_count_ >= 10) → context_manager.ClearAll() + reset
    4. fps_update_interval (60s) 마다 realtime_fps 갱신
    5. for engine in detection_engines:
         FrameFormattingContext ctx (각 engine 의 input resolution 별)
         ctx.Convert(frame, capture_save_image=true, generate_inference_blob=true)
              └─ sws_scale (FFmpeg) → letterbox padded RGB → blob
         req = engine.BuildRequest(&ctx, correlation_id)
         load_balancer_->RequestAsync(move(req))           ← engine_input_q 에 push
         result = load_balancer_->RespondAsync(subscribe_id, remaining_budget)
                                                          ← ReplyDispatcher 로 unit별 응답 매칭
    6. ConvertInferObjectsCoordinates(infer_objects, padded_style, original_style)
                                                          ← I3 InferObject 좌표 변환
    7. Tracker:
       trackers_[class_id]->TrackObjects(objects, request_seq)
                                                          ← SORT (Kalman+Hungarian)
    8. 이상행동:
       for sch in scheduler_:
         sch->Check(...)                                  ← AbnormalActionChecker
       if (event triggered):
         this->SendDetectResultToMetaData(detect_results) ← I1 RTSP proxy meta out
         BuildNotifyJsonImpl_Analysis(...)
         sio_handler_->Emit(...)                          ← F4 SocketIO
         frame_path = MakeImageSavePath(...)
         io_work_queue_->enqueue_move(IOWorkItem{ frame_path, save_snapshot_mat.clone() })
                                                          ← cv::imwrite 비동기 (F4)
         metrics: events_total++

  [is_schedule_updated_ atomic flag 체크]
    F2 callback 이 schedule_settings_ 갱신 시 set → 다음 loop 에서 ResetTrackers + 새 scheduler_ 빌드
```

### 3.3 EngineLoadBalancer 의 단일 NPU 분배

```
EngineLoadBalancer (단일 RKNN NPU 환경):
  - engine_input_q_ : sptr<SafeQueue<InputLayerWrapper>>     (단일)
  - infer_respond_receiver_ : sptr<SafeQueue<OutputLayerWrapper>> (단일, 모든 응답 모임)
  - reply_dispatcher_ : ReplyDispatcherWithCleaner<Output, UnitID>
  - reply_dispatcher_thread_ : SafeThread

[Linker (Init 시 NPU engine 이 호출)]
  Linker(handle_uuid, input_q_from_engine):
    engine_uuid_ = handle_uuid
    engine_input_q_ = input_q_from_engine
    engine_linked_ = true
    return infer_respond_receiver_     ← engine 이 응답을 여기에 push

[RtspDetectorUnit 호출]
  Subscribe(unit_id) → subscribers_.insert(unit_id)
  RequestAsync(req):
    if engine_input_q_.size() >= 128: return false (drop)
    engine_input_q_->enqueue_move(req)
  RespondAsync(unit_id, timeout):
    return reply_dispatcher_.wait_and_get(unit_id, timeout)

[reply_dispatcher_thread (Runner)]
  while running:
    output = infer_respond_receiver_->dequeue_wait_for(100ms)
    if has_value:
      reply_dispatcher_.set_reply(output->meta_data.requester_unit_id, *output)
        └─ unit별 cv 깨움
```

핵심 패턴:
- 단일 NPU → 단일 input_q. 모든 카메라가 한 큐에 push
- 응답은 단일 receiver_q 에 모임 → ReplyDispatcher 가 unit_id 기준 분배
- backpressure: input_q max=128. 초과 시 RequestAsync 가 false 반환 → 카메라가 frame 1개 drop

### 3.4 NPU EngineHandler 의 추론 thread

```
EngineHandlerBase::ActivateEngine():
  InitializeDevice()                                       ← NPU: no-op
  LoadModelEngineFile()                                    ← rknn_init(model_data)
  AllocateBuffers()                                        ← rknn_query(IN_OUT_NUM/ATTR), 입출력 버퍼 사전 할당
  Link()
    └─ engine_linker_(handle_uuid, request_q_) → respond_q_
  inference_thread_.Start()

EngineHandlerBase::InferenceThreadRunner():
  while running:
    input = request_q_->dequeue_wait_for(...)              ← LoadBalancer 에서 들어옴
    if Preprocess(input) returns true (배치 가득):
      DoInference()                                         ← rknn_run(ctx) (동기)
      outputs = Postprocess()                               ← rknn_outputs_get → YOLOv5 디코드 + NMS
      for out in outputs: respond_q_->enqueue_move(out)    ← LoadBalancer 의 receiver_q
    else:
      continue (배치 채워질 때까지 누적)
    rknn_outputs_release(...)
```

### 3.5 SORTTracker

```
SORTTracker(in_style, out_style):
  hungarian_ = make_unique<MgenHungarianAlgorithm>()
  kalmans_ = []  (per active track 1개)

TrackObjects(objects, request_seq):
  lock_guard(mtx_)
  predicted_boxes_ = [k->Predict() for k in kalmans_]
  cost_matrix_ = IOU between predicted_boxes_ and objects (>= iou_threshold)
  assignment_ = hungarian_->Solve(cost_matrix_)
  for matched_pair: kalmans_[trk]->Update(objects[det].bbox)
  for unmatched objects: 새 Kalman 생성 (id = kalman_tracker_create_count_++)
  for unmatched tracks (age > max_age_): erase
  return [InferObject with track_id assigned]
```

mutex 는 defensive (현재 단일 thread 호출이지만 미래 대비, 헤더 코멘트에 명시).

### 3.6 AbnormalActionChecker

```
EventChecker = function<DataLayer(const DataLayer&, Schedule*)>

알고리즘 4개:
- IntrusionLine    (사람 라인 침입)
- IntrusionZone    (사람 영역 침입)
- VehicleIntrusion (차량 라인 침입)
- VehicleParking   (차량 영역 체류 — loitering 시간 기반)

각 Schedule 인스턴스가:
- ROI (line/area)
- weekly + start_time + range_minutes
- HistoryChecker<T> (per track 의 시간 누적 데이터)
- notification_min_interval_sec (재발생 throttle)

Check() 호출:
  weekly/time 검사 → 외부면 skip
  HistoryChecker.GetHistory(track_id).Update()
  알고리즘 실행 → DataLayer (events list)
  HistoryChecker.ReleaseInactiveHistory(check_target_objects)
  notification_min_interval_sec 검사 → emit 가능 여부 결정
```

### 3.7 IOWorker (F6 Disk Defense 와 결합)

```
IOWorkerThreadRunner (RtspDetectorUnit.cpp:1290~):
  while running:
    opt_item = io_work_queue_->dequeue_wait_for(1s)

    [L2-Regular] every 1h: CleanupOldFrameDirs(7d) → metrics
    [L2-Emergency] every 5min: EmergencyCleanupIfDiskHigh() (≥80%)
    [L1] before imwrite: GetFrameDiskUsedPercent() ≥ 90% → skip
    cv::imwrite(frame_path, image_mat)

io_work_queue_ max=30 → drop oldest (P40 패턴)
```

### 3.8 RtspDetectorUnit::Stop / Destroy 순서 (UAF 방지)

```
Stop():
  inference_thread_.Stop()             ← Closer = avframe_q_->terminate()
  io_worker_thread_.Stop()             ← Closer = io_work_queue_->terminate() + clear
  load_balancer_->Unsubscribe(subscribe_id)

~RtspDetectorUnit():
  ClearAllSubscriptions()              ← F-F2-03 fix: callback의 [this] dangling 차단
  ReleaseSchedules()
  ResetTrackers()
  // 멤버들 destroy
```

`ClearAllSubscriptions` 가 dtor 의 첫 줄에 있어 SettingMonitor 의 callback 이 destroy 중인 멤버에 접근하는 race 차단.

---

## §4. Lifetime & Ownership

| 객체 | 소유자 | 수명 |
|---|---|---|
| `RtspDetectorBlock` | `Service_DETECTOR::detector_block_` (unique_ptr) | F1 service 종속 |
| `RtspDetectorUnit` | `service_units_` (vector<unique_ptr>) | block 종속 |
| `inference_thread_` / `io_worker_thread_` | RtspDetectorUnit 멤버 (value) | unit 종속. dtor 에서 자동 Stop+join |
| `avframe_q_` | RtspDetectorUnit 멤버 (`sptrSafeQueue`) | unit 종속. **CRtspProxy 도 ptr 보유 (setDecodedFrameSafeQueue)** — proxy 가 살아있는 한 push 가능 |
| `io_work_queue_` | unit 멤버 (unique_ptr) | unit 종속 |
| `proxy_ptr_` (`CRtspProxy*`) | I1 RTSP 라이브러리 (g_rtsp_cfg.proxy linked list) | RTSP 라이브러리 종속. unit 은 raw ptr 만 |
| `engine_input_q_` (`sptrSafeQueue<Input>`) | EngineHandlerBase 가 만들고 LoadBalancer 가 보유 | engine 종속 |
| `infer_respond_receiver_` | EngineLoadBalancer 멤버 | balancer 종속 |
| `reply_dispatcher_` | EngineLoadBalancer 멤버 (value) | balancer 종속. dtor 에서 StopAutoCleanup |
| Subscription unregister lambda | RtspDetectorUnit (SettingMonitor::active_subscriptions_) | unit 종속 |
| `trackers_` (`unordered_map<class, unique_ptr<SORTTracker>>`) | unit 멤버 | unit 종속 |
| `scheduler_` (`vector<shared_ptr<Schedule>>`) | unit 멤버 | unit 종속. ResetTrackers 시 재구성 |
| `image_data` (`shared_ptr<vector<uchar>>` in InputLayerWrapper) | engine 가 사용 끝나면 release. shared_ptr 로 multi-thread 안전 | shared_ptr refcount |
| `IOWorkItem.image_mat` | IOWorkItem 안 cv::Mat (clone deep copy) | enqueue 후 IOWorker thread 가 imwrite 후 destroy |

핵심 패턴:
- `avframe_q_` 가 RtspDetectorUnit 과 CRtspProxy 가 공유하는 **shared 큐** — RTSP proxy 가 producer, unit 의 inference_thread 가 consumer
- 이 shared 관계로 인해 종료 순서 중요: unit 의 inference_thread 정지 후 proxy 가 stop 되어야 dangling write 없음 (반대도 가능 — terminate 가 wait 깨움)

---

## §5. Concurrency

### Per camera 의 Thread 구성

| Thread | 역할 |
|---|---|
| InferenceThread (RtspDetectorUnit) | 메인 루프. dequeue avframe → 추론 → 트래킹 → abnormal → emit |
| IOWorkerThread (RtspDetectorUnit) | cv::imwrite 비동기 처리 |

### Process-wide Thread

| Thread | 역할 |
|---|---|
| RTSP rx_thread (I1) | RTSP 패킷 수신 |
| RTSP task (I1) | RTSP 메인 루프 |
| Per-proxy decode thread (I1 내부) | H.264/H.265 디코딩 → setDecodedFrameSafeQueue |
| EngineHandlerBase::inference_thread_ | NPU 추론 루프 (전체 1개) |
| EngineLoadBalancer::reply_dispatcher_thread_ | infer_respond_receiver_ 에서 dispatch |
| InferenceCounter::thread_ | 10초 간격 DFPS 합산 |
| ReplyDispatcherWithCleaner cleanup thread | timeout entry 정리 |
| F4 SocketIO thread / F5 GRPC threads / F6 logger reopen 등 | 다른 흐름 |

### 주요 락

| 락 | 보호 대상 |
|---|---|
| `RtspDetectorUnit::exclude_setting_mtx_` | `exclude_setting_` (F2 callback writer + main loop reader) |
| `RtspDetectorUnit::schedule_settings_mtx` | `schedule_settings_` (F2 callback writer + main loop reader). flag `is_schedule_updated_` atomic 으로 reset signal |
| `SORTTracker::mtx_` | TrackObjects (defensive) |
| `EngineLoadBalancer::engine_mutex_` | engine_uuid_ / engine_input_q_ / engine_linked_ |
| `EngineLoadBalancer::unit_regist_mutex_` | subscribers_ |
| `IOStreamManager::map_mutex_` | 모든 큐/dispatcher map |
| `InferenceCounter::mutex_` | counters_ map |

### Backpressure 패턴

| 큐 | 한계 | drop 정책 |
|---|---|---|
| `avframe_q_` | 무제한 (default) | 단, RTSP proxy 의 `setDecodedFrameSafeQueue(true, fps_limit)` 가 fps 제한 |
| `engine_input_q_` (LoadBalancer) | 128 | size 검사 후 RequestAsync false → unit 이 frame 1개 drop |
| `io_work_queue_` | 30 | enqueue_move 가 oldest drop |

---

## §6. Findings

### F-F3-01 — `avframe_q_` capacity 무제한 → RTSP burst 시 메모리 누적 가능
- **등급**: WARN
- **위치**: [RtspDetectorUnit.cpp:346](../../code/Main/DETECTOR/src/RtspDetectorUnit.cpp#L346) (코멘트는 "Capacity 30 (cv::Mat ~6MB × 30 = ~180MB)" 인데 실제 코드는 SetMaxSize 호출 유무 확인 필요)
- **내용**: 초기화 코드 grep 결과 `io_work_queue_` 는 30 명시했지만 `avframe_q_` 에는 SetMaxSize 호출이 보이지 않음. RTSP 측이 fps_limit 으로 throttle 하지만 inference thread 가 멈추면 누적 가능.
- **현 영향**: AVFrame 은 shared_ptr 로 RTSP proxy 가 알아서 새 frame 생성 시 oldest drop 가능 (proxy 의 `setDecodedFrameSafeQueue(true, fps_limit)` 의 첫 인자 의미). I1 라이브러리 내부 동작 검증 필요.
- **권장**: `avframe_q_->SetMaxSize(2 * fps_limit)` 명시적 설정 추가. 이미 RTSP 측이 처리한다면 코멘트로 명시.

### F-F3-02 — `RequestAsync` backpressure 시 frame drop 의 메트릭 부재
- **등급**: NOTE
- **위치**: [RtspDetectorUnit.cpp:1032](../../code/Main/DETECTOR/src/RtspDetectorUnit.cpp#L1032)
- **내용**: `RequestAsync` false 반환 시 frame drop 인데 `errors_total{type="engine_input_full"}` 같은 카운터 부재.
- **현 영향**: NPU bottleneck 발생 시 외부 관측 불가.
- **권장**: drop 시 메트릭 increment 추가. F-I3-03 와 결합 처리 가능.

### F-F3-03 — `SORTTracker` 의 `mtx_` 가 defensive — 현재 사용 패턴이 단일 thread 면 불필요
- **등급**: INFO
- **위치**: [SORTTracker.h:72-74](../../code/Tracker/SORT/SORTTracker.h#L72-L74)
- **내용**: 헤더 코멘트에 "현재 단일 thread (RtspDetectorUnit Loop) 만 호출". 현재 그 패턴 유지.
- **권장**: 변경 없음. 미래 대비.

### F-F3-04 — `AbnormalActionChecker::Schedule::Check` 의 throw 가능성 (검증 필요)
- **등급**: NOTE
- **위치**: AbnormalActions/src/* (라인 단위 미검토)
- **내용**: nlohmann::json 접근 / std::stoi 등 throw 가능 함수 호출 시 catch 누락하면 main loop 종료. 1차 리뷰의 γ-2 fix 가 DETECTOR.cpp ExtractUnitFromJson 에 stoi try/catch 추가했으므로 비슷한 패턴이 다른 곳에도 있을 가능성.
- **권장**: AbnormalActions 내부 라인 단위 검사 (deferred — F3 이후).

### F-F3-05 — `MAGIC_DETECTION_ENGINE_NAME` 하드코딩 ("DetectionEngine")
- **등급**: NOTE
- **위치**: [RtspDetectorUnit.h:41](../../code/Main/DETECTOR/include/RtspDetectorUnit.h#L41)
- **내용**: 단일 분기 (Detection) 가정. 미래 다른 모델 (Pose, Segmentation) 추가 시 분기 확장 필요.
- **권장**: 변경 없음. 베이스 프로젝트 의도.

### F-F3-06 — `consecutive_mismatch_count_ < 10` 임계치로 해상도 노이즈 보호
- **등급**: INFO (긍정 발견)
- **위치**: [RtspDetectorUnit.h:44](../../code/Main/DETECTOR/include/RtspDetectorUnit.h#L44), [RtspDetectorUnit.cpp:861-885](../../code/Main/DETECTOR/src/RtspDetectorUnit.cpp#L861)
- **내용**: 일시적 해상도 변경(노이즈)에 대해 10 frame 연속 후에 reset. SwsContext 재생성을 빈번히 하지 않게 보호.

### F-F3-07 — `is_schedule_updated_` atomic flag 패턴 — F2 callback 의 lock-free signal
- **등급**: INFO (긍정 발견)
- **위치**: [RtspDetectorUnit.h:173](../../code/Main/DETECTOR/include/RtspDetectorUnit.h#L173)
- **내용**: F2 callback 이 schedule_settings_mtx 잡고 갱신 → atomic flag set. main loop 가 flag 검사 → ResetTrackers + 새 scheduler 빌드. callback 자체는 락 짧게 잡고 즉시 반환. main loop 의 schedule reset 비용을 callback thread 에 부담시키지 않음.

### F-F3-08 — `RtspDetectorUnit::Init` 의 검증 chain (load_balancer/network/rtsp/sio/io_stream 5중)
- **등급**: INFO (긍정 발견)
- **위치**: [RtspDetectorUnit.cpp:299-346](../../code/Main/DETECTOR/src/RtspDetectorUnit.cpp#L299)
- **내용**: 모든 의존 manager 가 nullptr 아님을 명시 검증. 누락 시 명확한 에러 로그.

### F-F3-09 — `EngineLoadBalancer::engine_input_q_` size 검사 → RequestAsync 의 race
- **등급**: NOTE
- **위치**: [EngineLoadBalancer 의 RequestAsync 구현]
- **내용**: size 검사 → enqueue 사이에 다른 thread 가 enqueue 하면 한순간 capacity 초과 가능 (race window). 단, 한도 128 이 hard limit 이 아니라 backpressure threshold 라 영향 거의 없음. 검증 필요.
- **권장**: SafeQueue 의 `SetMaxSize(128)` 사용으로 enqueue 단위 atomic 처리 가능.

### F-F3-10 — RKNN context 가 inference_thread 단일 thread 에서만 호출 — F-I2 검증
- **등급**: INFO (긍정 발견)
- **위치**: [EngineHandlerBase 의 InferenceThreadRunner]
- **내용**: I2 의 검증 항목이었던 "ctx-per-thread 패턴" — 단일 inference_thread 가 모든 RKNN API 호출. 직렬화 보장.

### F-F3-11 — `setDecodedFrameSafeQueue(avframe_q_, true, detect_fps_limit_)` — RTSP 라이브러리에 unit 의 큐 ptr 전달
- **등급**: WARN (검증 필요)
- **위치**: [RtspDetectorUnit.cpp:786](../../code/Main/DETECTOR/src/RtspDetectorUnit.cpp#L786)
- **내용**: I1 라이브러리가 unit 의 큐를 보유. unit 이 destroy 되어도 라이브러리가 push 시도하면 UAF.
- **현 영향**: avframe_q_ 가 shared_ptr 이라 unit 보유 ref + I1 보유 ref. unit destroy 시에도 큐는 살아있음. terminate 호출로 RTSP 가 push 멈추는지 검증 필요.
- **권장**: Stop() 시 `proxy_ptr_->setDecodedFrameSafeQueue(nullptr)` 같은 unlink API 가 있는지 확인. 없다면 추가. 또는 proxy 자체 stop 후 unit destroy 순서 보장 (현재 F1 Quit 순서가 #03 detector_block stop → #04 network close → 즉 detector 먼저 멈추고 RTSP 가 나중에 멈춤 → 그 사이 RTSP 가 terminated avframe_q 에 push 시도 가능).

### F-F3-12 — `MAX_ENGINE_INFER_INPUT_QUEUE_SIZE = 128` 이 anonymous namespace 상수
- **등급**: INFO
- **위치**: [EngineLoadBalancer.h:22](../../code/Management/manager/include/EngineLoadBalancer.h#L22)
- **내용**: ServerSettingData 에 `inference_per_cams_fps_limit` 같은 동적 설정이 있는데 큐 크기는 하드코딩.
- **권장**: 카메라 수 * 4 같은 동적 산출 또는 ServerSetting 에 설정 추가. 미래 확장.

### F-F3-13 — `frame_count_` atomic 만 증가, 실제 사용처 없음 (검증 필요)
- **등급**: INFO
- **위치**: [RtspDetectorUnit.h:151](../../code/Main/DETECTOR/include/RtspDetectorUnit.h#L151)
- **내용**: 멤버는 정의되어 있지만 cpp 에서 활용 grep 필요. dead code 가능성.
- **권장**: 사용처 grep 후 dead 면 제거.

### F-F3-14 — `IsOverTime` 검사가 wall clock (system_clock) 기반
- **등급**: NOTE
- **위치**: [RtspDetectorUnit.cpp:817-819](../../code/Main/DETECTOR/src/RtspDetectorUnit.cpp#L817)
- **내용**: NTP jump / 시스템 시간 변경 시 한 번 잘못된 시간 비교 가능. AbnormalActions::HistoryChecker 는 steady_clock 사용 (good).
- **권장**: long_timelapse_log_interval 검사도 steady_clock 권장. 영향 매우 낮음.

### F-F3-15 — `subscribe_ids_` set 이 unsubscribe 시점 명확하지 않음
- **등급**: NOTE
- **위치**: [RtspDetectorUnit.h:164](../../code/Main/DETECTOR/include/RtspDetectorUnit.h#L164), [.cpp:761-766](../../code/Main/DETECTOR/src/RtspDetectorUnit.cpp#L761)
- **내용**: InferenceThreadRunner setup 에서 subscribers_ 추가. unit 종료 시 LoadBalancer.Unsubscribe 호출 위치는 Stop()/dtor 둘 중 검증 필요.
- **권장**: 라인 단위 확인 (deferred).

---

## §7. Open Questions

1. **F-F3-01 / F-F3-11**: `avframe_q_` 의 capacity 와 RTSP proxy 가 unit destroy 후에도 push 시도하는지 — I1 라이브러리 내부 동작 확인 필요. 위험도가 큰 항목.
2. **F-F3-04**: AbnormalActions 내부 throw 가능 함수의 catch 처리 누락 여부 — 라인 단위 검토 가치 있음.
3. **F-F3-13**: `frame_count_` 가 실제 사용되는지.

---

## §8. Self-Check

- [x] Primary 파일 읽음 — RtspDetectorBlock {h,cpp} / RtspDetectorUnit.h / RtspDetectorUnit.cpp 핵심 부분 (Init/Run/Stop/InferenceThreadRunner setup + main loop 시작 + IOWorker) / Engine/EngineBase.h / EngineLoadBalancer.h / IOStreamManager.h / EngineClient.h / InferenceCounter.h / SORTTracker.h / AbnormalActionChecker.h / HistoryChecker.h / FramePreProcessor.h
- [x] §3 호출 시퀀스 — F1→F3 진입 / InferenceThreadRunner 메인 루프 / EngineLoadBalancer / SORTTracker / Abnormal / IOWorker / Stop 순서 모두 file:line
- [x] §4 소유권 — unique_ptr / shared_ptr / shared 큐 / I1 raw ptr 모두 명시
- [x] §5 동시성 — per-camera thread 2개 + process-wide thread 6개 + 7개 락 정리
- [x] §6 Finding 등급 + 출처 (15개)
- [x] 추측 표시 — "검증 필요" (F-F3-01 SetMaxSize, F-F3-04 throw 패턴, F-F3-13 frame_count_, F-F3-15 unsubscribe 시점)
- [x] Also-touches 라벨 모순 없음

**검증 결과**: PASS

**Stage F-base 의 보강 항목 검증 결과**:
- F-I3-01 (SafeQueue::dequeue throw) — **F3 에서 dequeue 사용처 0건** (모두 dequeue_wait_for). 영향 없음 → WARN 강등 가능
- F-I3-04 (ReplyDispatcher vs WithCleaner) — F3 는 **WithCleaner 사용** (EngineLoadBalancer::reply_dispatcher_) → 안전
- F-I3-02 (SafeThread closer 미설정) — RtspDetectorUnit 의 inference_thread / io_worker_thread 모두 closer 설정. EngineLoadBalancer / EngineHandlerBase / InferenceCounter 도 같은 패턴 추정 (`SetThreadFunctions(Runner, Closer)`)
- F-F2-07 (CameraSettingData 동적 변경) — F3 는 ScheduleSettingData / ExcludeCamSettingData 만 subscribe. 카메라 추가/제거는 process restart 가정 — 의도된 결정
- F-F6-08 (다중 IOWorker race) — 카메라 N대 → IOWorker N개. 동일 `/frame` 트리에서 동시 cleanup 가능 (확인). std::error_code silent ignore 로 안전성 확보. 단 race window 잔존 (NOTE 유지)

**보강 필요 항목 (F4·F5 로 인계)**:
- F-F3-11: avframe_q_ unit ↔ RTSP proxy 의 lifecycle 정합성 (큰 위험)
- F-F3-04: AbnormalActions 내부 throw 패턴
- F-F3-15: LoadBalancer.Unsubscribe 호출 시점
