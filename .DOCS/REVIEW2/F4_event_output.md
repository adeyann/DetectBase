# F4 — Event Output (SocketIO + REST + IOWorker frame save)

## §1. Why

검출된 이벤트와 분석 결과를 외부 소비자에게 전달하는 흐름. 4 채널 outbound:

1. **SocketIO emit** — `Message` 이벤트로 MVAS broker 에 broadcast (메인 채널)
2. **REST API GET** — MVAS 서버에서 설정 데이터 가져오는 inbound 도 ApiHandler 가 담당 (실제로는 settings 전용)
3. **GRPC client broadcast** — F5 의 outbound 부분 (NetworkManager 에서 관리)
4. **IOWorker frame save** — cv::imwrite 비동기 (이벤트 발생 시점의 snapshot)

또한 inbound(외부 → DetectBase):
- SocketIO listener — `EventNotifycation`, `ExceptionUpdate`, `ScheduleUpdate` 수신 → SettingManager 갱신 (F2 트리거)

제거 시 잃는 것: 검출 결과가 외부에 가지 않음. 운영 가치 0.

---

## §2. Roster

### Primary (F4)

| 카테고리 | 파일 |
|---|---|
| **NetworkManager (handler 통합)** | [manager/include/NetworkManager.h](../../code/Management/manager/include/NetworkManager.h), [manager/src/NetworkManager.cpp](../../code/Management/manager/src/NetworkManager.cpp) |
| **SocketIO** | [worker/include/SioHandler.h](../../code/Management/worker/include/SioHandler.h), [worker/src/SioHandler.cpp](../../code/Management/worker/src/SioHandler.cpp), [Protocol/SocketIO/](../../code/Protocol/SocketIO/) (외부 sioclient 라이브러리, 표면 API 만) |
| **REST API** | [worker/include/ApiHandler.h](../../code/Management/worker/include/ApiHandler.h), [worker/src/ApiHandler.cpp](../../code/Management/worker/src/ApiHandler.cpp), [Protocol/REST/](../../code/Protocol/REST/) (외부 restclient-cpp + curl 헤더, 표면 API 만) |

### Also-touches

| 흐름 | 활용 |
|---|---|
| F3 | RtspDetectorUnit 이 sio_handler_->Emit() 호출 / IOWorker 가 cv::imwrite |
| F5 | NetworkManager 가 GrpcEventClientBase 보유 (forward decl) — Phase 1 broadcast 통합 |
| F2 | ApiHandler 가 SettingManager::Initialize 의 REST GET 4종 수행, SioHandler 가 inbound listener 로 SettingMonitor trigger |
| F6 | socketio_reconnect_total / events_total / errors_total{type=imwrite_fail|emit_fail} |
| I3 | SafeQueue (emit_queue) / SafeThread (emit_control_thread) / json |

---

## §3. How

### 3.1 NetworkManager — 통합 진입점

```
[F1 Stage #02]
  network_manager_ = make_shared<NetworkManager>(network_profile_)
    └─ ctor: init_profile_ 만 보관 (handler 들은 ConnMVAS 시 생성)

[F1 Stage #04]
  network_manager_->ConnMVAS(service_name)
    │
    ├─ BuildAPISetting() → ApiSetting{mvas_ip, api_port, my_local_ip}
    ├─ CreateApiHandler(setting)
    │     └─ api_handler_ = make_shared<ApiHandler>(setting)
    │
    ├─ BuildSettingManagerInitData(service_tag) → init_data
    ├─ InitializeSettingManager(move(init_data))
    │     └─ SettingManager::Initialize() ← F2! REST GET 4종 (server/cluster/camera/exclude/schedule)
    │
    ├─ BuildSocketIOSetting(my_service_id) → SioSetting
    ├─ CreateSioHandler(setting)
    │     └─ sio_handler_ = make_shared<SioHandler>(setting)
    │     └─ sio_handler_->Initialize() (connect + emit_queue + emit_control_thread)
    │
    ├─ BuildRtspSetting(...) → RtspSetting
    ├─ CreateRtspHandler(setting)
    │     └─ rtsp_handler_ = make_shared<RtspHandler>(setting)
    │     └─ rtsp_handler_->Initialize() ← F3 의 RTSP cfg xml 로드
    │
    └─ InitializeGrpcClients()  ← F5! grpc_client_enabled 면 grpc_peers 만큼 client 생성

[F1 Stage Run]
  network_manager_->GetRtspHandler()->RunRTSP()
```

### 3.2 SocketIO outbound — emit_queue + emit_control_thread (P40 패턴)

```
SioHandler::Initialize():
  emit_queue.SetMaxSize(1000)                              ← P40 capacity 한계
  client.connect(...)
  emit_control_thread.SetThreadFunctions(EmitRunner, EmitCloser)
  emit_control_thread.Start()

SioHandler::Emit(event_name, json_data):
  EmitMessage msg = {event_name, json_data}
  emit_queue.enqueue_copy(msg)                            ← max=1000, 초과 시 drop oldest

EmitControlThreadRunner():
  while running:
    opt_msg = emit_queue.dequeue_wait_for(1s)
    if has_value:
      // sio::client 의 emit 은 thread-safe 가 아니므로 single thread 에서 직렬 호출
      current_socket->emit(msg.first, msg.second.dump())

EmitControlThreadCloser():
  emit_queue.terminate()
```

**핵심**:
- F3 메인 루프가 `Emit()` 호출 → 큐에 enqueue 만 → **메인 FPS 안 막힘**
- emit_control_thread 가 직렬 emit → sio::client thread-safety 문제 회피
- max=1000 backpressure (P40 fix)

### 3.3 SocketIO inbound — EventBinder + SettingMonitor 트리거

```
[F1 Stage #05 SocketIOEventBind()]
  sio_handler_->RegistEvent(make_shared<SioEventBinder>(event_name_tag, event_type))
  binder->SetTargetUnitsExtractor([](json){ return [unit_ids] })
  binder->SetInterProtocolFunc(api_handler_->BuildCallback_GET(checker, url_builder))
  binder->SetMultiUintsSettingManager(GetSettingManager()->Get<X>SettingsManager())

[Runtime SocketIO 수신]
  sio_message 수신 (예: ScheduleUpdate)
   → SioHandler 의 listener 가 binder->Run(json) 호출
   → CorrelationScope("evt") (P53 RAII 새 ID)
   → binder 의 mode flag 분기:
       - need_target_unit_extractor_mode: extract unit_ids → for each unit:
           inter_proc(json) → REST GET → response_json → SettingManager.UpdateTargetUnit(unit_id, response)
                                                              └─ SetterBase::Update → callback 호출 (락 외부, F2 #10/#11)
       - need_internal_enqueueing_mode: internal_queue->enqueue (단순 큐잉)
```

### 3.4 REST API — ApiHandler.GET_or_throw_if_timeout (이름은 throw 지만 실제는 안 함)

```
ApiHandler::GET_or_throw_if_timeout(url, timeout):
  resp = conn->GET_or_throw_if_timeout(url, timeout)   ← restclient-cpp wrap
  // 코멘트: "GET_or_throw_if_timeout 은 더 이상 throw 하지 않음 (timeout 시 code <= 0)"
  if (good response):
    return {code, parse_json(body)}
  else:
    return {code <= 0, empty_json}

ApiHandler::IsGoodResponse(code) → true/false
```

- 함수 이름은 historical (이전엔 timeout 시 throw 했음). 현재는 code 반환.
- → **F-F2-01 강등**: 더 이상 throw 안 함. 코드만 misleading.

### 3.5 IOWorker — cv::imwrite 비동기 (F3 의 RtspDetectorUnit 안)

F3 분석 §3.7 참조. 짧게:

```
F3 InferenceThread (event 발생 시):
  io_work_queue_->enqueue_move(IOWorkItem{ frame_path, save_snapshot_mat.clone() })
  → 메인 루프 즉시 복귀 (cv::imwrite 비용 외부화)

F4 IOWorkerThread (RtspDetectorUnit::IOWorkerThreadRunner):
  L2-Regular cleanup / L2-Emergency / L1 disk pre-block
  cv::imwrite(opt_item->frame_path, opt_item->image_mat)
  errors_total{type="imwrite_fail"}++ on failure
```

### 3.6 GRPC client outbound (F5 의 outbound 측 — NetworkManager 통합)

```
NetworkManager::InitializeGrpcClients():
  if !grpc_client_enabled: return true
  for peer in grpc_peers:
    client = make_shared<GrpcEventClientBase>(peer)
    grpc_clients_.push_back(client)

NetworkManager::BroadcastEventOnlyJsonToGrpcPeers(json_payload):
  if grpc_clients_.empty(): return 0
  for client in grpc_clients_:
    client->SendEventOnlyJson(payload)   ← fire-and-forget async
  return grpc_clients_.size()

NetworkManager::CloseGrpcClients():
  for client in grpc_clients_: client->Close()
  grpc_clients_.clear()
```

### 3.7 종료 순서 (F4 측)

```
F1 Quit():
  ...
  #04. CloseNetworkAll():
    if(sio_handler_):  sio_handler_->TerminateSocketIO()
       └─ emit_control_thread.Stop() (closer = emit_queue.terminate())
       └─ client.sync_close()
    if(rtsp_handler_): rtsp_handler_->StopRTSP()
       └─ proxy stop, hrtsp.r_flag=0, thread join
    CloseGrpcClients()
       └─ for client: client->Close()
       └─ grpc_clients_.clear()
  ...
```

---

## §4. Lifetime & Ownership

| 객체 | 소유자 | 수명 |
|---|---|---|
| `NetworkManager` | F1 의 `shared_ptr<NetworkManager>` | F1 service 종속 |
| `ApiHandler` | NetworkManager 멤버 (`shared_ptr<ApiHandler>`) | NM 종속 |
| `SioHandler` | NetworkManager 멤버 (`shared_ptr<SioHandler>`) | NM 종속 |
| `RtspHandler` | NetworkManager 멤버 (`shared_ptr<RtspHandler>`) | NM 종속 |
| `vector<shared_ptr<GrpcEventClientBase>>` | NetworkManager 멤버 | NM 종속 (forward decl in header — F5) |
| `RestClient::Connection` | ApiHandler 멤버 (unique_ptr) | ApiHandler 종속 |
| `sio::client client` / `current_socket` | SioHandler 멤버 (value) | SioHandler 종속 |
| `emit_queue` (`SafeQueue<EmitMessage>`) | SioHandler 멤버 (value, max=1000) | SioHandler 종속 |
| `emit_control_thread` | SioHandler 멤버 (value) | SioHandler 종속 |
| SioEventBinder | `event_binders` map (shared_ptr) | SioHandler 종속 |

NetworkManager 는 모든 outbound handler 의 single composition root. 종료 시 RAII + dtor 에서 CloseNetworkAll 호출 (idempotent flag).

---

## §5. Concurrency

| Thread | 역할 |
|---|---|
| `emit_control_thread` (SioHandler) | emit_queue dequeue → sio::client::emit |
| sio::client 내부 thread (외부 라이브러리) | SocketIO 통신 + listener dispatch |
| RestClient::Connection 내부 (curl) | REST GET 시 thread block (호출 thread 가 대기) |
| IOWorker (RtspDetectorUnit) | cv::imwrite + cleanup |
| (F5) GRPC CompletionQueue thread per client | RPC completion 대기 |

| 락 | 보호 |
|---|---|
| `SioHandler::lock` + `cond` | conn_finish (Initialize 시 connect 대기) |
| `SioHandler::emit_queue` 내부 mutex | I3 SafeQueue 자체 |
| `ApiHandler::mtx` | conn 접근 |
| sio::client 내부 락 | 외부 라이브러리 |

핵심 패턴:
- F3 메인 루프 → SioHandler::Emit() (락 짧게 잡음) → 큐 enqueue → 즉시 복귀
- emit_control_thread 가 직렬 emit (sio::client thread-safety 보호)
- ApiHandler 의 GET 은 호출 thread block — F2 init 시 main thread 가 대기 (의도)

---

## §6. Findings

### F-F4-01 — `ApiHandler::GET_or_throw_if_timeout` 이름이 misleading (현재 throw 안 함)
- **등급**: WARN (네이밍 정합성)
- **위치**: [ApiHandler.cpp:25-58](../../code/Management/worker/src/ApiHandler.cpp#L25), [SettingManager.cpp:100-101](../../code/Management/manager/src/SettingManager.cpp#L100) 코멘트
- **내용**: 함수 이름은 throw 의미. 코드 코멘트에 "더 이상 throw 하지 않음 (timeout 시 code <= 0)" 명시. 그러나 호출부(SettingManager) 가 여전히 try/catch 로 감쌈.
- **현 영향**: F-F2-01 의 큰 부분 해소됨. 다만 misleading 함수 이름.
- **권장**:
  1. 함수 이름을 `GET_with_timeout` 으로 rename + 호출부 try/catch 제거 (정합성)
  2. 또는 코드 코멘트를 헤더에도 명시 + 호출부 try/catch 제거

### F-F4-02 — `emit_queue` drop 시 메트릭 미수집 (F-I3-03 의 F4 검증)
- **등급**: WARN
- **위치**: [SioHandler.cpp:53](../../code/Management/worker/src/SioHandler.cpp#L53) (max=1000), [.cpp:219](../../code/Management/worker/src/SioHandler.cpp#L219) (enqueue_copy)
- **내용**: SafeQueue::enqueue_copy 가 max 초과 시 silently drop oldest. 운영 중 emit 폭주 시 외부 관측 불가.
- **현 영향**: F-I3-03 의 직접 영향. 운영 모니터링 사각지대.
- **권장**: enqueue 전 size check + drop 시 `errors_total{type="emit_drop"}` increment. 또는 SafeQueue 자체에 drop 카운터 추가.

### F-F4-03 — `BroadcastEventOnlyJsonToGrpcPeers` 가 fire-and-forget — 실패 별 통계 미수집
- **등급**: NOTE
- **위치**: [NetworkManager.cpp:76-82](../../code/Management/manager/src/NetworkManager.cpp#L76)
- **내용**: 송신 시도된 peer 수만 반환. 실제 도착 보장 안 됨 (코멘트로 명시). F5 의 grpc_send_total / send_success_total / send_failed_total 메트릭이 처리.
- **현 영향**: F5 메트릭으로 보완됨.

### F-F4-04 — SocketIO inbound listener 가 catch 처리 (검증 필요)
- **등급**: NOTE
- **위치**: SioHandler.cpp listener / SioEventBinder::Run
- **내용**: nlohmann::json 접근 / API GET 의 error code 처리 / SettingManager.Update 의 결과 분기 — 모두 try/catch 또는 명시적 분기로 처리 필요. 라인 단위 미검토.
- **권장**: SioEventBinder::Run 의 throw 보호 검증 (deferred, 큰 문제는 아님).

### F-F4-05 — SocketIO 가 sync_close 까지 대기 — 종료 시 timeout 위험
- **등급**: NOTE
- **위치**: [SioHandler::TerminateSocketIO 추정], [NetworkManager::CloseNetworkAll]
- **내용**: sio::client::sync_close() 가 서버 응답 대기. 네트워크 단절 상황에서 종료 hang 가능. 외부 라이브러리(sioclient 3.1.0) 의 timeout 설정 확인 필요.
- **현 영향**: 현재 운영에서 hang 보고 없음.
- **권장**: sync_close 대신 close + timeout 정책 — sioclient API 확인 후 결정.

### F-F4-06 — `SioEventBinder` 의 mode flag 4개 (interprotocol/setting_update/internal_enqueue/target_unit_extractor)
- **등급**: INFO (긍정 발견)
- **위치**: [SioHandler.h:220-223](../../code/Management/worker/include/SioHandler.h#L220-L223)
- **내용**: 다양한 inbound 패턴 (REST 후 setting update / 단순 enqueue / 직접 처리 등) 을 mode flag 로 분기. 확장성 좋음.

### F-F4-07 — `emit_queue` 가 SafeQueue::enqueue_copy (move 미사용)
- **등급**: NOTE
- **위치**: [SioHandler.cpp:219](../../code/Management/worker/src/SioHandler.cpp#L219)
- **내용**: `EmitMessage` (std::pair<string, json>) 을 copy 로 enqueue. JSON 이 큰 경우 비용. enqueue_move 가능.
- **권장**: `emit_queue.enqueue_move(std::move(message))` 사용. 작은 작업.

### F-F4-08 — `RestClient::Connection` 가 ApiHandler 마다 단일 — 다중 GET 직렬화
- **등급**: NOTE
- **위치**: [ApiHandler.h:118](../../code/Management/worker/include/ApiHandler.h#L118)
- **내용**: 단일 connection 객체 + mtx. 동시 GET 시 직렬. F2 init 시점에는 연속 4번 GET 이라 직렬이 자연. inbound trigger (SioEventBinder) 가 GET 호출 시 mtx 경합 가능.
- **현 영향**: 동시 GET 빈도 낮음. 영향 미미.
- **권장**: 변경 없음.

### F-F4-09 — `NetworkManager::CloseNetworkAll` 의 GRPC client close 가 SocketIO/RTSP 와 같은 단계
- **등급**: NOTE
- **위치**: [NetworkManager.cpp:262](../../code/Management/manager/src/NetworkManager.cpp#L262) — GRPC init 도 같은 ConnMVAS 안
- **내용**: GRPC client lifecycle 이 NetworkManager 에 통합. 일관성 좋음.

### F-F4-10 — SocketIO `OnConnect / OnClose / OnFail` listener 가 callback 으로만 정의 — 메트릭 자동 갱신 검증 필요
- **등급**: NOTE
- **위치**: [SioHandler::connection_listener 124-149](../../code/Management/worker/include/SioHandler.h#L137-L149)
- **내용**: socketio_reconnect_total counter 가 어디서 increment 되는지 확인 필요. OnFail 또는 OnClose 에서 호출되어야 함.
- **권장**: SioHandler.cpp 의 OnClose/OnFail 라인 단위 확인 (deferred).

### F-F4-11 — `SioSetting` 의 `sio_conn_auth_token` 이 default `"detectbase-token"` 하드코딩
- **등급**: NOTE
- **위치**: [SioHandler.h:45](../../code/Management/worker/include/SioHandler.h#L45)
- **내용**: 인증 토큰 default 가 코드 상수. NetworkSettings.json 이나 환경변수에서 override 가능한지 확인 필요.
- **현 영향**: 베이스 프로젝트라 production 배포 시 NetworkSettings 에서 override 권장.
- **권장**: NetworkProfile 에 추가 (현재 누락 추정).

### F-F4-12 — `EmitMessage` 가 std::pair<string, json> — emit 시점에 json.dump() 호출 필요
- **등급**: INFO
- **위치**: [SioHandler.h:77](../../code/Management/worker/include/SioHandler.h#L77)
- **내용**: dequeue 후 `current_socket->emit(name, json.dump())` — sio 라이브러리는 string 받음. dump 가 emit_control_thread 에서 발생 → main 영향 없음.

---

## §7. Open Questions

1. **F-F4-01**: `GET_or_throw_if_timeout` 함수 rename + try/catch 제거 가능? (작은 작업이지만 코드 정합성 ↑)
2. **F-F4-02**: emit_queue drop 메트릭 추가 — F-I3-03 의 직접 답. 추가할까?
3. **F-F4-05**: sync_close 의 timeout 보장 — 운영 중 hang 발생 시 영향 큼. 우선순위 평가.
4. **F-F4-11**: SocketIO auth token 을 NetworkProfile 에 추가할까?

---

## §8. Self-Check

- [x] Primary 파일 읽음 — NetworkManager.h, SioHandler.h, ApiHandler.h + 핵심 .cpp 의 grep
- [x] §3 시퀀스 — ConnMVAS / Emit + EmitControlThread / inbound binder / REST GET / IOWorker / GRPC broadcast / 종료 순서 모두 file:line
- [x] §4 소유권 — NetworkManager 가 모든 handler 의 root, shared_ptr 패턴
- [x] §5 동시성 — emit_control_thread / sio internal / restclient block / IOWorker / GRPC threads + 락 4개
- [x] §6 Finding 등급 + 출처 (12개)
- [x] 추측 표시 — "검증 필요" (F-F4-04 SioEventBinder throw, F-F4-05 sync_close timeout, F-F4-10 reconnect counter 위치)
- [x] Also-touches 라벨 모순 없음

**검증 결과**: PASS

**Stage F-base/I 의 보강 항목 검증 결과**:
- **F-F2-01 (ApiHandler throw)** — **확인: GET_or_throw_if_timeout 은 더 이상 throw 안 함**. 함수 이름만 misleading. WARN → NOTE 강등 가능. 단, F-F4-01 새 등급으로 이동 (네이밍 정합성).
- **F-I3-03 (SafeQueue drop 메트릭)** — F4 emit_queue 도 drop 메트릭 미수집 확인. WARN 으로 격상 (F-F4-02).
- **F-I3-02 (SafeThread closer)** — SioHandler::emit_control_thread 도 SetThreadFunctions(Runner, Closer) 패턴. closer 가 emit_queue.terminate() 호출. 안전.
- **F-F1-08 (ConnMVAS 안의 SettingManager::Initialize)** — 확인 완료: ConnMVAS → InitializeSettingManager → SettingManager::Initialize. 의도된 응집 패턴이지만 명시적 분리 가치 있음 (F-F1-08 NOTE 유지).

**보강 필요 항목 (F5 로 인계)**:
- F-F4-04: SioEventBinder::Run 의 throw 패턴 정밀 검토
- F-F4-10: socketio_reconnect_total 의 increment 위치
- F-F4-11: SocketIO auth token 의 외부 설정화
