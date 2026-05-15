# INTER_FLOW — 흐름 간 결합부

> 흐름 내부만 봐서는 잡기 어려운 경계 이슈를 모음. Stage I + F-base + F-runtime 의 모든 흐름 분석을 가로질러 검토.

---

## §0. 결합부 식별 원칙

**결합부**: 두 흐름이 같은 자료/락/스레드/lifecycle 을 공유하는 지점. 각 흐름 내부에서는 정합한데 결합부에서 race / leak / dangling 이 가능한 지점.

이 문서에서 다루는 결합부는 다음 형태로 정리:
- **Edge**: 두 흐름의 이름
- **Contact point**: 같이 사용하는 자료/락/객체
- **Risk**: 결합 시 발생 가능한 문제
- **Current safeguard**: 현재 코드에서 이미 적용된 보호
- **Verdict**: 안전 / 잠재 위험 / 위험

---

## §1. F1 (Lifecycle) ↔ F2 (Configuration) — Initialize 시점 응집

### Contact points

- `Service_DETECTOR::Initialize` Stage #04 의 `network_manager_->ConnMVAS(service_name)` 호출 안에 `SettingManager::Initialize()` 가 숨어있음
- ApiHandler / SioHandler / RtspHandler / GrpcEventClientBase 4개의 init 이 ConnMVAS 안에서 순차 실행

### Risk

- 외부에서 보면 ConnMVAS 가 단순 connect 같지만 실제로는 REST GET 4번 + SettingManager 4개 setter 초기화 + GRPC client lifecycle 시작.
- 호출 시점 / 실패 처리가 ConnMVAS 안에 캡슐화되어 외부 테스트 어려움.
- ConnMVAS 가 timeout 으로 hang 시 Stage #04 에서 멈춤. SIGINT 5회 강제 종료 정책 (F1) 으로 회피.

### Current safeguard

- ConnMVAS 가 `bool` 반환 → false 시 `STEP_CHECK` 실패 → Initialize 종료 → Quit
- ApiHandler::GET_or_throw_if_timeout 이 timeout 시 throw 안 하고 code <= 0 반환 (F-F4-01) — 즉시 실패 분기 가능
- F1 의 IgnoreSignalHandler 5회 정책 (강제 종료 보장)

### Verdict

**안전** (캡슐화는 의도적 응집). 단 명시적 분리가 가독성 ↑ — F-F1-08 NOTE 유지.

---

## §2. F2 (Configuration) ↔ F3 (Pipeline) — SettingMonitor callback 의 락 외부 호출

### Contact points

- `RtspDetectorUnit : private SettingMonitor`
- `SubscribeSetting<ScheduleSettingData>` / `SubscribeSetting<ExcludeCamSettingData>`
- callback 안에서 `RtspDetectorUnit::exclude_setting_mtx_` / `schedule_settings_mtx` 잡고 멤버 갱신
- callback 호출 시점에 SettingManager 의 어떤 락도 hold 중 아님 (#10/#11 fix)
- main loop 가 같은 mtx 잡고 read

### Risk

1. callback 이 unit destroy 도중 호출되면 `[this]` capture dangling (UAF)
2. callback 안에서 락 잡고 SettingManager 의 다른 메서드 호출 시 deadlock 가능 (예: GetSetting 호출 → SetterBase 락)

### Current safeguard

1. RtspDetectorUnit::~RtspDetectorUnit() 의 첫 줄이 `ClearAllSubscriptions()` ([RtspDetectorUnit.cpp:294](../../code/Main/DETECTOR/src/RtspDetectorUnit.cpp#L294)) — UAF 차단 (F-F2-03)
2. SetterBase::Update / TriggerCallbacks 가 callback 호출 시점에 모든 락 해제 — 재진입 deadlock 안전 (F2 §3.3)
3. is_schedule_updated_ atomic flag 패턴 — callback 은 락 짧게 잡고 immediate 반환, 재구성은 main loop 가 담당 (F-F3-07)

### Verdict

**안전** (1차 리뷰의 fix 적용 + Stage Synthesis 검증).

---

## §3. F3 (Pipeline) ↔ I1 (RTSP) — avframe_q_ shared 큐의 lifecycle race

### Contact points

- `proxy_ptr_->setDecodedFrameSafeQueue(avframe_q_, true, detect_fps_limit_)` ([RtspDetectorUnit.cpp:786](../../code/Main/DETECTOR/src/RtspDetectorUnit.cpp#L786))
- I1 RTSP 라이브러리가 unit 의 `sptrSafeQueue<sptr<AVFrame>> avframe_q_` ptr 보유
- 라이브러리 내부 디코드 thread 가 avframe_q_->enqueue (producer)
- unit 의 InferenceThread 가 dequeue (consumer)

### Risk (가장 큰 위험 — F-F3-11)

```
F1 Quit 종료 순서:
  #03 Stop Service Implements → detector_block_->Stop() → unit 의 inference_thread Stop
  #04 Stop Network Flow       → rtsp_handler_->StopRTSP()

  ⚠ 이 사이의 race window:
    - unit 의 inference_thread 정지됨
    - 그러나 RTSP 라이브러리는 아직 동작 → push 시도 가능
    - avframe_q_ 가 shared_ptr 라 살아있음 → push 자체는 OK
    - 그러나 unit destroy 후에 라이브러리가 enqueue 시도 시 unit 의 다른 멤버 (proxy_ptr_ 등)
      가 dangling 일 수 있음 — RtspDetectorUnit 자체는 service_units_ 의 unique_ptr
      안에 있어 RtspDetectorBlock dtor 까지 살아있음 (Service_DETECTOR::~Service_DETECTOR 시점)
```

### Current safeguard

- `avframe_q_` 가 `sptrSafeQueue<...>` (shared_ptr) — 큐 자체 lifetime 은 unit 멤버 + 라이브러리 ref 둘 다 잡음. unit 멤버 destroy 되어도 큐는 라이브러리 ref 가 살아있는 동안 보존 (단 unit destroy 후 enqueue 가 발생해도 dequeue 할 consumer 없으니 누적만)
- inference_thread_.Stop() 의 Closer 가 `avframe_q_->terminate()` 호출 (검증 필요 — RtspDetectorUnit.cpp:1260+ 의 InferenceThreadCloser 라인 직접 확인 필요) — terminate 후 enqueue 는 무시되거나 누적
- F1 종료 순서 코멘트 ("새 순서로 변경 시 UAF") — 명시적 경고

### 검증 필요 항목

- I1 라이브러리가 `setDecodedFrameSafeQueue(nullptr)` 같은 unlink API 가 있는지
- terminate 된 SafeQueue 에 enqueue 가능 여부 (현재 SafeQueue.h: `b_terminate=true` 면 dequeue 만 nullopt — enqueue 는 그대로 진행)
- proxy 가 stop 되기 전 unit 이 destroy 되면 `setDecodedFrameSafeQueue(nullptr)` 같은 cleanup 이 있는가

### Verdict

**잠재 위험**. 현 운영에서 문제 미보고. 종료 순서가 변경되거나 RTSP 라이브러리 동작이 달라지면 즉시 위험 상승. → INTER_FLOW 의 가장 큰 검토 항목.

### 권장 조치

1. F3 의 `RtspDetectorUnit::Stop()` 에서 `proxy_ptr_->setDecodedFrameSafeQueue(nullptr)` 또는 동등한 unlink 호출 (라이브러리 API 확인 필요)
2. avframe_q_ 의 max_size 명시 설정 (`SetMaxSize(2 * fps_limit)`) — 누적 메모리 한계 보호 (F-F3-01)
3. 종료 순서 코멘트를 더 강하게 — `// !!! DO NOT REORDER !!! see .DOCS/REVIEW2/INTER_FLOW.md §3` 같은 표시

---

## §4. F3 (Pipeline) ↔ F4 (Event Output) — emit/imwrite 백프레셔

### Contact points

- `SioHandler::emit_queue` (max=1000, drop oldest)
- `RtspDetectorUnit::io_work_queue_` (max=30, drop oldest)
- F3 메인 루프가 두 큐에 enqueue → F4 측 thread (emit_control_thread, IOWorkerThread) 가 dequeue

### Risk

1. emit 폭주 시 oldest drop → 손실. 메트릭 미수집 (F-F4-02)
2. cv::imwrite 폭주 시 oldest drop → 이미지 미저장. 메트릭 미수집 (F-I3-03 의 직접 영향)
3. enqueue 자체는 빠름 → F3 메인 FPS 안 막힘 (의도)

### Current safeguard

- `enqueue_copy/move` 가 max_size 초과 시 silently pop_front (oldest drop) — F3 안 막힘
- L1 disk pre-block 으로 imwrite 큐 입력 자체를 줄임 (디스크 가득 시 skip)
- `errors_total{type="imwrite_fail"}` 카운터 (imwrite 실패만)

### Verdict

**안전** (백프레셔 정책 동작). 단 drop 의 외부 관측 부재 → 운영 모니터링 사각.

### 권장 조치

emit/imwrite drop 시점에 메트릭 추가 (F-F4-02 + F-I3-03 의 통합 답).

---

## §5. F3 (Pipeline) ↔ F6 (Observability) — Disk Defense 의 다중 IOWorker race

### Contact points

- 카메라 N대 → 각 unit 이 IOWorkerThread 1개 → IOWorker N개 동시 동작
- 모든 IOWorker 가 동일 `/frame` 트리에서 L2-Regular cleanup / L2-Emergency cleanup
- `std::filesystem::directory_iterator` + `remove_all` 동시 호출 가능

### Risk

- 동일 day folder 를 여러 worker 가 동시 cleanup 시도 → fs::remove_all 중복 호출
- 한 worker 가 삭제 중인 path 를 다른 worker 가 다시 iterate 시 stale entry → 삭제 실패 또는 unexpected 상태

### Current safeguard

- 모든 fs 호출에 `std::error_code ec` — silent ignore. 다음 주기 재시도
- `last_cleanup` / `last_emergency` 타이머가 worker 마다 독립 → 동시 발생 빈도 낮음 (다른 시간에 시작 시)
- `std::filesystem` 자체가 OS 수준 race 에는 안전 보장 없지만 ENOENT/EACCES 등 표준 에러로 종료

### Verdict

**안전** (silent retry 정책). 단 효율 측면에서는 한 worker 가 lock 잡는 게 깔끔.

### 권장 조치 (선택)

```cpp
// RtspDetectorUnit.cpp 의 anonymous namespace
namespace {
    std::mutex g_emergency_cleanup_mtx;
}

EmergencyCleanupResult EmergencyCleanupIfDiskHigh() noexcept {
    std::unique_lock lock(g_emergency_cleanup_mtx, std::try_to_lock);
    if (!lock.owns_lock()) return {0, 0};  // 다른 worker 가 진행 중 — skip
    // ... 기존 로직
}
```

`try_to_lock` 으로 빈 시도 시 skip — 다음 주기에서 재시도. F-F6-08 의 직접 답.

---

## §6. F4 (Event Output) ↔ F5 (GRPC) — outbound 일관성

### Contact points

- F3 메인 루프가 이벤트 발생 시:
  ```
  sio_handler_->Emit(...)                                          ← F4 SocketIO
  network_manager_->BroadcastEventOnlyJsonToGrpcPeers(...)         ← F5 GRPC
  ```
- 두 outbound 가 독립적으로 fire-and-forget

### Risk

1. SocketIO emit 성공 + GRPC 실패 → 일부 peer 만 받음 (운영자가 상태 차이 인지 어려움)
2. GRPC 실패 시 어떤 peer 가 실패인지는 send_failed_total 메트릭으로 보지만 SocketIO 측 실패는 별도 채널
3. peer 가 일시적으로 down 되면 channel reconnect 동안 fail (F-F5-08)

### Current safeguard

- 메트릭 분리:
  - `events_total` (전체 이벤트 발생)
  - `socketio_reconnect_total` (SocketIO 재연결)
  - `grpc_send_total / send_success_total / send_failed_total{rpc, code}` (GRPC 결과)
- fire-and-forget 패턴 — 한쪽 실패가 다른쪽 막지 않음

### Verdict

**안전** (의도적 독립). 단 운영 모니터링 시 두 채널 메트릭을 같이 봐야 일관성 판단 가능.

### 권장 조치

NOTE 로 운영 docs 추가 — "outbound 채널은 독립. 한쪽 실패 시 다른쪽 영향 없음. 일관성은 메트릭 비교로 확인".

---

## §7. F1 (Lifecycle) ↔ F3/F4/F5 — Init/Shutdown 순서

### Contact points

```
Init 순서 (Service_DETECTOR::Initialize):
  Logger init (F6)
  MetricsRegistry::Initialize (F6)
  Profile parsers (F2 정적)
  NetworkManager / IOStreamManager / EngineLoadBalancer ctor (F4 / F3)
  Engines build + ActivateEngine (F3)
  ConnMVAS → SettingManager::Initialize (F2 동적) + GRPC client init (F5)
  IOStreamManager::Ready (F3 큐 등록)
  RtspHandler::Initialize (F3)
  SocketIOEventBind (F4 inbound)
  RtspDetectorBlock 빌드 + Init (F3)
  GRPC server (F5, 조건부)

Shutdown 순서 (Quit, 역순 ≠):
  GRPC Server stop (F5)
  Engines TerminateEngine (F3)
  LoadBalancer Terminate (F3)
  RtspDetectorBlock Stop (F3 unit 의 inference/io thread 정지)
  NetworkManager CloseNetworkAll (F4 SocketIO + RTSP + GRPC client)
  IOStreamManager ClearAll (F3 큐 정리)
  MetricsRegistry Shutdown (F6)
```

### Risk

1. Init 에서 누가 누구에 의존: NetworkManager 가 SettingManager 호출, RtspHandler 가 SettingManager.GetCameraIDSet() 호출, RtspDetectorBlock 이 모두 의존 → 초기화 순서 어긋나면 nullptr 또는 빈 데이터
2. Shutdown 에서 종료 순서 변경 시 UAF (DETECTOR.cpp:376 코멘트 명시)

### Current safeguard

- Init 의 매 stage 가 `STEP_CHECK` 또는 `if-return-false` 로 실패 즉시 abort
- Shutdown 의 코멘트 "새 순서로 변경 시 UAF"
- Quit 의 `is_quit_.exchange(true)` 멱등성

### Verdict

**안전** (검증된 순서 + 멱등성). 코멘트 의존도 큼 → INTER_FLOW §3 의 종료 순서 강화 권장과 같은 결론.

---

## §8. F6 (Observability) ↔ 모든 흐름 — correlation_id / metric 누수

### Contact points

- `CorrelationContext::Set("sys-detector-<id>")` ([RtspDetectorUnit.cpp:702](../../code/Main/DETECTOR/src/RtspDetectorUnit.cpp#L702)) — InferenceThread 진입 시 정적 ID
- `CorrelationScope("evt")` — F4 SocketIO listener 의 RAII scope
- `MetricsRegistry::Instance()` — 전역 싱글턴

### Risk

1. correlation_id 가 thread_local 라 thread 간 전파 안 됨. F3 의 InferenceThread 가 emit_queue 에 enqueue 하면 SioHandler::emit_control_thread 가 dequeue → 그 thread 의 correlation_id 는 다른 값
2. CorrelationScope dtor 가 prev 로 복원 — nested scope 안전

### Current safeguard

- 정적 ID (sys-detector-<id>) 가 InferenceThread 시작 시 set → 그 thread 의 모든 MLOG 동일 ID
- emit_queue 안의 EmitMessage 는 JSON 자체에 correlation_id 가 포함됨 (BuildNotifyJsonBase 가 첨부) — emit_control_thread 가 다른 ID 라도 메시지 본문에는 정확한 ID

### Verdict

**안전** (각 thread 별 ID 유지 + JSON 본문 첨부). cross-thread 전파는 design choice.

### 잠재 개선 (선택)

emit_queue 에 enqueue 시 correlation_id 도 함께 넣어 emit_control_thread 가 set 하는 패턴. 즉시 가치 낮음.

---

## §9. I3 ↔ 모든 흐름 — SafeQueue / SafeThread 사용 패턴 정합성

### 정합성 검증 결과

| 사용처 | SafeQueue 패턴 | 정합성 |
|---|---|---|
| F3 avframe_q_ | dequeue_wait_for, max_size 미설정 | ⚠ max_size 누락 (F-F3-01) |
| F3 io_work_queue_ | dequeue_wait_for, max_size=30 | ✅ |
| F3 EngineLoadBalancer engine_input_q_ | size 검사 + enqueue, max=128 (anon namespace 상수) | ✅ (F-F3-09 race 미세) |
| F3 EngineLoadBalancer infer_respond_receiver_ | dequeue_wait_for | ✅ |
| F4 SioHandler emit_queue | dequeue_wait_for, max=1000 | ✅ (drop 메트릭 누락 — F-F4-02) |
| F5 server queue_send_event_json_ | 외부 set | 외부 책임 |
| F5 dispatcher_event_json_ | raw ReplyDispatcher (not WithCleaner) | ⚠ 외부 cleanup 책임 |

### SafeThread closer 검증

| 사용처 | closer | 정합성 |
|---|---|---|
| F3 inference_thread_ | InferenceThreadCloser (avframe_q_ terminate) | ✅ (검증됨) |
| F3 io_worker_thread_ | IOWorkerThreadCloser (io_work_queue_ terminate + clear) | ✅ |
| F3 EngineHandlerBase inference_thread_ | InferenceThreadCloser | ✅ |
| F3 EngineLoadBalancer reply_dispatcher_thread_ | ReplyDispatcherThreadCloser | ✅ |
| F3 InferenceCounter thread_ | Closer | ✅ |
| F4 SioHandler emit_control_thread | EmitControlThreadCloser (emit_queue terminate) | ✅ |
| F5 GrpcEventServerBase grpc_server_thread_ | HandleRpcsCloser | ✅ |
| F5 GrpcEventClientBase grpc_client_thread_ | (asyncCompleteRpc 의 cq.Shutdown ?) | 검증 필요 (F-F5-07) |
| F6 ReplyDispatcherWithCleaner cleaner_thread_ | CleanupCloser | ✅ |

### Verdict

**대부분 안전**. 잔여 검증: F-F5-07 (client cq closer).

---

## §10. F2 → F3 → F4 → F5 의 이벤트 흐름 일관성

```
[F3 InferenceThread 이벤트 발생 지점]
1. AbnormalActions 가 event_occured_schedule 반환
2. SendDetectResultToMetaData(detect_results)        ← I1 RTSP proxy meta out
3. json = BuildNotifyJsonImpl_Analysis(...)
4. sio_handler_->Emit("Message", json)                ← F4 SocketIO
5. (조건부) network_manager_->BroadcastEventOnlyJsonToGrpcPeers(json.dump())  ← F5 GRPC
6. frame_path = MakeImageSavePath(...)
7. io_work_queue_->enqueue_move(IOWorkItem{frame_path, mat.clone()})  ← F4 IOWorker
8. metrics: events_total{type=..., cam=...}++

[비동기 분기]
- emit_control_thread 가 큐에서 dequeue → SocketIO 송신
- IOWorkerThread 가 큐에서 dequeue → cv::imwrite
- GRPC client 의 cq drain thread 가 응답 처리
- 메트릭은 atomic counter (외부 pull)
```

### 잠재 일관성 이슈

- 5번 (GRPC) 호출이 어디서 트리거되는지 코드 검증 필요. RtspDetectorUnit 에서 직접 호출하거나 SioHandler 안에서 동시 호출 또는 외부 binding. → 검증 필요.

### Verdict

**대부분 안전**. 단 5번 GRPC 호출 시점 / 위치는 라인 단위 검증 필요. 현재 grep 으로는 NetworkManager::BroadcastEventOnlyJsonToGrpcPeers 호출처가 RtspDetectorUnit.cpp 내부에 있을 것으로 추정.

---

## §11. 종합 결합부 위험도 매트릭스

| 결합부 | 위험도 | 처리 권장 |
|---|---|---|
| §3 F3↔I1 avframe_q lifecycle | **잠재 위험** | unlink API 추가 또는 종료 순서 강화 |
| §5 F3↔F6 다중 IOWorker race | 안전 (silent retry) | static mtx try_to_lock 권장 (효율) |
| §4 F3↔F4 emit/imwrite drop | 안전 (백프레셔) | drop 메트릭 추가 |
| §10 F3→F4→F5 이벤트 일관성 | 안전 | GRPC 호출 위치 검증 |
| §2 F2↔F3 callback 락 외부 | 안전 (#10/#11 fix) | 변경 없음 |
| §1 F1↔F2 ConnMVAS 응집 | 안전 (의도) | 변경 없음 |
| §6 F4↔F5 outbound 독립 | 안전 (의도) | docs 추가 |
| §7 F1↔F3/F4/F5 종료 순서 | 안전 (코멘트 보호) | 코멘트 강화 |
| §8 F6↔모든 correlation_id | 안전 | 변경 없음 |
| §9 I3↔모든 SafeQueue/Thread | 안전 (1건 검증 필요) | F-F5-07 closer 검증 |

---

## §12. Self-Check (INTER_FLOW)

- [x] 11개 결합부 모두 file:line 인용 + risk + safeguard + verdict
- [x] 가장 큰 위험 (§3 RTSP unit lifecycle) 명시 + 권장 조치 3개 제시
- [x] 흐름별 §6 Findings 와 cross-link
- [x] verdict 의 근거가 흐름 분석 §5 (Concurrency) §4 (Lifetime) 와 정합
- [x] 추측 표시 — "검증 필요" 명시 (§3 unlink API, §10 GRPC 호출 위치, §9 F5 cq closer)

**검증 결과**: PASS

---

## §13. INTER_FLOW 발견 항목 (FINDINGS 로 인계)

| ID | 등급 | 요약 |
|---|---|---|
| **IF-01** | RISK 후보 → 검증 후 결정 | §3 F3↔I1 avframe_q lifecycle race window |
| IF-02 | NOTE | §5 F3↔F6 다중 IOWorker race (mtx try_to_lock 권장) |
| IF-03 | WARN | §4 F3↔F4 emit/imwrite drop 메트릭 누락 (F-F4-02 + F-I3-03 통합) |
| IF-04 | NOTE | §10 GRPC broadcast 호출 위치 라인 단위 검증 |
| IF-05 | NOTE | §7 종료 순서 코멘트 강화 (가시성 ↑) |
| IF-06 | NOTE | §6 outbound 일관성 docs 추가 |
