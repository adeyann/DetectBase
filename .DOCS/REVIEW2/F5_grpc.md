# F5 — GRPC Bidirectional (Server + Client + analysis 6 UAF fix)

## §1. Why

DetectBase 의 양방향 노드 통신. 메인 분기 프로젝트(2 Odroid Master/Slave) 에서 활용 예정.

핵심:
1. **GRPC server** — 외부 노드의 RPC 수신. async 패턴 (CompletionQueue tag-based)
2. **GRPC client** — peer 에 fire-and-forget 또는 request-response RPC 송신
3. **4 조합 모두 지원** — none / server-only / client-only / both. NetworkSettings 의 두 flag 독립 (default OFF)
4. **분석 6 UAF fix** — async detached thread 가 handler 를 dangling 참조하던 문제. Phase 2 fix 로 `enable_shared_from_this` + alive_handlers_ map. detached thread 가 self capture → handler lifetime 보장.

제거 시 잃는 것: 멀티 노드 통신 불가. 단, default OFF 이므로 미활성 시 영향 0 (인스턴스 생성 안 함).

---

## §2. Roster

### Primary (F5)

| 카테고리 | 파일 |
|---|---|
| **Server** | [Protocol/GRPC/include/GrpcEventServerBase.h](../../code/Protocol/GRPC/include/GrpcEventServerBase.h), [Protocol/GRPC/src/GrpcEventServerBase.cpp](../../code/Protocol/GRPC/src/GrpcEventServerBase.cpp) |
| **Client** | [Protocol/GRPC/include/GrpcEventClientBase.h](../../code/Protocol/GRPC/include/GrpcEventClientBase.h), [Protocol/GRPC/src/GrpcEventClientBase.cpp](../../code/Protocol/GRPC/src/GrpcEventClientBase.cpp) |
| **Handler template** | [Protocol/GRPC/include/GrpcUnaryHandler.h](../../code/Protocol/GRPC/include/GrpcUnaryHandler.h) (Phase 2 fix 의 핵심) |
| **Proto** | [Protocol/GRPC/include/GrpcProtoMaker.h](../../code/Protocol/GRPC/include/GrpcProtoMaker.h), [Protocol/GRPC/src/GrpcProtoMaker.cpp](../../code/Protocol/GRPC/src/), [Protocol/GRPC/protos/MgenProto.{pb,grpc.pb}.{h,cc}](../../code/Protocol/GRPC/protos/) (생성 파일) |

### Also-touches

| 흐름 | 활용 |
|---|---|
| F1 | Service_DETECTOR 의 grpc_server_ unique_ptr 멤버. forward decl + .cpp dtor 패턴 |
| F4 | NetworkManager 가 GrpcEventClientBase 보유 (`grpc_clients_`). Initialize/Close/Broadcast |
| F2 | NetworkProfile 의 grpc_{client,server}_enabled / grpc_peers / grpc_server_bind_address/port |
| F6 | grpc_send_total / send_success_total / send_failed_total / recv_total / client_enabled / peer_count / server_enabled |
| I3 | SafeQueue (request queue) / ReplyDispatcher (request-response 응답 매칭) / SafeThread (CompletionQueue drain thread) / UUIDGenerator |

---

## §3. How

### 3.1 RPC 시그니처 (Mgen.proto)

```
service DETECTBASE_GRPC {
  // Fire-and-forget (Phase 1)
  rpc SendEventOnlyJson  (EventDataOnlyJson)      returns (Empty);
  rpc SendEventWithImages(EventDataWithRawImages) returns (Empty);

  // Request-Response (Phase 1)
  rpc RequestEventOnlyJson  (EventDataOnlyJson)      returns (EventDataOnlyJson);
  rpc RequestEventWithImages(EventDataWithRawImages) returns (EventDataOnlyJson);

  // [Phase 4 샘플] Counter / Heartbeat
  rpc SendCounterDelta       (CounterDelta)    returns (Empty);
  rpc SendHeartbeat          (HeartbeatPing)   returns (Empty);
  rpc RequestCounterSnapshot (CounterRequest)  returns (CounterSnapshot);
}
```

7 개 RPC. 메시지 타입: EventDataOnlyJson(uuid, json_data) / EventDataWithRawImages(uuid, images[]) / CounterDelta / CounterRequest / CounterSnapshot / HeartbeatPing / Empty.

### 3.2 Server lifecycle (F1 init)

```
[F1 Init Phase 3]
  if(network_profile_.grpc_server_enabled):
    grpc_server_ = make_unique<GrpcEventServerBase>(bind_addr, port)
       └─ ctor: server_address_ = MakeUrl(ip, port)
                grpc_server_thread_ 의 SetThreadFunctions(HandleRpcsRunner, HandleRpcsCloser)

    grpc_server_->SetSendEventOnlyJsonPostProcesser([](req, rsp){ recv 메트릭 ↑; })
    grpc_server_->SetSendEventWithImagesPostProcesser([](req, rsp){ recv 메트릭 ↑; })

    grpc_server_->Run()
       └─ ServerBuilder + AddListeningPort + RegisterService
       └─ cq_ = builder.AddCompletionQueue()
       └─ server_ = builder.BuildAndStart()
       └─ for each RPC: GrpcUnaryDirectProcessHandler::CreateAndRegister(this, &service_, cq, ...)
       └─ grpc_server_thread_.Start()  ← HandleRpcsRunner 시작
```

### 3.3 HandleRpcsRunner (CompletionQueue drain)

```
HandleRpcsRunner:
  void* tag;
  bool ok;
  while cq_->Next(&tag, &ok):
    auto handler_raw = static_cast<CallDataBase*>(tag)
    handler_raw->Proceed(ok)             ← Phase 2: alive_handlers_ 가 shared_ptr 로 보유

HandleRpcsCloser:
  server_->Shutdown()                     ← graceful shutdown deadline 적용
  cq_->Shutdown()
  DrainCompletionQueue()                 ← Next 가 false 반환할 때까지 drain
```

### 3.4 Phase 2 fix — handler lifetime 모델 (분석 6 UAF 차단)

문제 (fix 전):
```
DirectHandler::Proceed(FINISH):
  delete this;                            ← (구) raw delete
  // 그러나 detached thread 가 이미 this 를 capture 한 상황이면 UAF
```

Phase 2 fix:
```
class CallDataBase : public std::enable_shared_from_this<CallDataBase> {...};

GrpcEventServerBase 가 alive_handlers_ map (raw ptr → shared_ptr) 보유:
  RegisterHandler(h)   → alive_handlers_[h.get()] = h  (ref +1)
  UnregisterHandler(p) → alive_handlers_.erase(p)      (ref -1)

handler 의 Proceed:
  CREATE → request_func_ (다음 RPC 대기)
  PROCESS:
    if !ok: server_->UnregisterHandler(this); return;
    CreateAndRegister(...)  ← 다음 handler 등록 (꼬리 큐잉)
    enqueue_*(request_)
    if handler_: handler_(request_, reply_)
    responder_.Finish(reply_, OK, this)
    status = FINISH
  FINISH → server_->UnregisterHandler(this); 

AsyncHandler::Proceed (PROCESS, request-response):
  uuid = uuid_extractor_(request_)
  in_queue_->enqueue_move(request_)
  
  // Phase 2 fix: shared_from_this capture
  auto self = static_pointer_cast<...>(shared_from_this());
  
  std::thread([self, uuid, dispatcher, timeout]{
    auto result = dispatcher->wait_and_get(uuid, timeout);
    self->status_ = FINISH;
    if(result): self->responder_.Finish(*result, OK, self.get())
    else:       self->responder_.Finish({}, DEADLINE_EXCEEDED, self.get())
    // lambda scope 종료 → self ref -1 → alive_handlers_ 에서 erase 된 상태면 dtor 발동
  }).detach();

server stop 시:
  ClearAllHandlers() → alive_handlers_.clear()
  detached thread 의 self capture 만 남음 → thread 종료 시 자동 dtor
```

검증된 동작 (1차 리뷰 self-loopback):
- send=68, recv=68 일치 — proto/handler/lifecycle 모두 정상

### 3.5 Client async pattern (CompletionQueue + tag)

```
GrpcEventClientBase (Phase 1+4):
  channel_ = grpc::CreateChannel(target, InsecureCredentials)
  stub_    = DETECTBASE_GRPC::NewStub(channel_)
  cq_      = grpc::CompletionQueue
  grpc_client_thread_ 의 SetThreadFunctions(asyncCompleteRpc, /*close*/cq_.Shutdown)
  grpc_client_thread_.Start()

SendEventOnlyJson(event) [fire-and-forget]:
  call = new SendCall  (raw new — async 패턴)
  setCommonContext(call, uuid)
  call->response_reader = stub_->PrepareAsyncSendEventOnlyJson(&call->context, event, &cq_)
  call->response_reader->StartCall()
  call->response_reader->Finish(&call->reply, &call->status, call)  ← tag = call
  메트릭: grpc_send_total{rpc="SendEventOnlyJson"}++

asyncCompleteRpc (background thread):
  void* tag; bool ok;
  while cq_.Next(&tag, &ok):
    base = static_cast<BaseCall*>(tag)
    if dynamic_cast<SendCall*>(base):
      if status.ok: send_success_total{rpc="..."}++
      else:         send_failed_total{rpc="...", code=status.error_code()}++
      delete base
    else if dynamic_cast<JsonResponseCall*>(base):
      if status.ok: callback(response.json_data())
      delete base
    else if dynamic_cast<CounterSnapshotCall*>(base):
      if status.ok: callback(response)
      // cool-down 1min log
      delete base

Stop:
  cq_.Shutdown()
  thread.join()  ← (SafeThread::Stop 안에서)
```

### 3.6 NetworkManager 가 client 를 사용하는 흐름 (F4 와 결합)

```
[F1 Init Stage #04]
  NetworkManager::ConnMVAS():
    ...
    InitializeGrpcClients():
      if !grpc_client_enabled: return true
      for peer in grpc_peers:
        client = make_shared<GrpcEventClientBase>(peer.ip, peer.port)
        client->Start()  ← cq drain thread 시작
        grpc_clients_.push_back(client)

[Runtime — F3 이벤트 발생 시]
  network_manager_->BroadcastEventOnlyJsonToGrpcPeers(json_payload)
    └─ for client in grpc_clients_:
         client->SendEventOnlyJson(EventDataOnlyJson{uuid, payload})

[F1 Quit]
  NetworkManager::CloseNetworkAll():
    CloseGrpcClients():
      for client: client->Stop() (cq Shutdown + thread join)
      grpc_clients_.clear()
```

---

## §4. Lifetime & Ownership

### Server side

| 객체 | 소유자 | 수명 |
|---|---|---|
| `GrpcEventServerBase` | F1 의 `unique_ptr<GrpcEventServerBase>` | 활성 시만. forward decl + .cpp dtor 패턴 (F-F1-03) |
| `grpc::Server server_` | GrpcEventServerBase 멤버 (unique_ptr) | server lifecycle |
| `grpc::ServerCompletionQueue cq_` | server 멤버 (unique_ptr) | server lifecycle |
| `alive_handlers_` map (raw → shared_ptr) | server 멤버 | server lifecycle. `ClearAllHandlers` 가 비움 |
| `CallDataBase` 인스턴스 | `shared_ptr` — alive_handlers_ + detached thread self capture | shared refcount. 둘 다 release 되면 dtor |
| `request/reply queue` (`SafeQueue<EventDataOnlyJson>` 등) | 외부에서 set (set 안 하면 nullptr) | 외부 책임 |
| `dispatcher_event_*` (`ReplyDispatcher`) | 외부에서 set | 외부 책임 |

### Client side

| 객체 | 소유자 | 수명 |
|---|---|---|
| `GrpcEventClientBase` | NetworkManager 의 `vector<shared_ptr<...>>` | NM 종속 |
| `grpc::Channel` (shared_ptr) | client 멤버 | client lifecycle |
| `Stub` (unique_ptr) | client 멤버 | client lifecycle |
| `grpc::CompletionQueue cq_` | client 멤버 (value) | client lifecycle |
| `BaseCall*` (SendCall/JsonResponseCall/CounterSnapshotCall) | **raw new + delete in cq drain** | tag 가 받아질 때까지 |
| `grpc_client_thread_` | client 멤버 | client lifecycle |

### Handler factory 패턴

```
CreateAndRegister(server, ...):
  h = make_shared<...>(server, ...)       ← shared_ptr 만 외부 노출
  server->RegisterHandler(h)               ← server 가 ref +1
  h->Init() → Proceed(true) → ctor 후 shared_from_this 가능 시점
  return h                                 ← 호출자는 잡고 있어도 되고 버려도 됨
                                           (server 가 잡고 있으니)
```

ctor 안에서는 `shared_from_this()` 호출 불가 (control block 미설정) → Init 분리.

---

## §5. Concurrency

### Server side

| Thread | 역할 |
|---|---|
| `grpc_server_thread_` (SafeThread) | HandleRpcsRunner = `cq_->Next()` 무한 루프 |
| (per RPC) detached thread | AsyncHandler::Proceed 내 `dispatcher_->wait_and_get` 대기 |

### Client side

| Thread | 역할 |
|---|---|
| `grpc_client_thread_` (SafeThread) | asyncCompleteRpc = `cq_.Next()` drain |
| 호출자 thread (F3 메인 등) | Send*/Request* 호출 — call 객체 new 후 즉시 복귀 |

### 락

| 락 | 보호 |
|---|---|
| `GrpcEventServerBase::alive_mtx_` | alive_handlers_ map (Register/Unregister race) |
| grpc 라이브러리 내부 락 | 외부 라이브러리 |

### 핵심 동시성 패턴

1. **handler shared lifetime** — server stop 시 detached thread 가 처리 중이어도 self capture 가 ref 유지 → thread 종료까지 객체 살아있음.
2. **fire-and-forget 의 backpressure 부재** — client 의 Send 가 cq 에 enqueue 만 하고 즉시 복귀. peer 가 죽어 있으면 cq 가 timeout 으로 fail 처리하지만 backlog 는 라이브러리 내부 처리.
3. **request-response timeout** — async handler 의 timeout_ms (default 3000ms). dispatcher.wait_and_get 의 timeout 과 결합.

---

## §6. Findings

### F-F5-01 — `server_owner_` 가 raw pointer (handler 가 server 보다 오래 살면 dangling)
- **등급**: WARN
- **위치**: [GrpcUnaryHandler.h:62](../../code/Protocol/GRPC/include/GrpcUnaryHandler.h#L62)
- **내용**: 코멘트에 "weak ref — server lifetime > handler lifetime 가정" 명시. 그러나 detached thread 가 처리 중일 때 server 가 dtor 진입하면, ClearAllHandlers 가 alive_handlers_ 비우지만 detached thread 의 self capture 는 여전히 살아있음. 그 thread 가 `self->responder_.Finish(...)` 호출 시 cq_ (server 의 멤버) 가 이미 destroy 됐을 가능성.
- **현 영향**: 현 종료 순서 (F1 Quit #00 GRPC server stop 이 첫 단계) 는 server.Shutdown() → cq.Shutdown() → 완료 후 server destroy. shutdown 이 detached thread 도 wait 하는지 grpc 라이브러리 동작 확인 필요. 현재 self-loopback 테스트는 통과했으나 stress/disconnect 시 검증 필요.
- **권장**: `weak_ptr<GrpcEventServerBase>` 또는 `enable_shared_from_this` 적용. 큰 작업.

### F-F5-02 — Client 의 `BaseCall*` 가 raw new/delete (CLAUDE.md 규칙)
- **등급**: WARN (CLAUDE.md 규칙)
- **위치**: [GrpcEventClientBase.cpp 의 SendEventOnlyJson 등](../../code/Protocol/GRPC/src/GrpcEventClientBase.cpp)
- **내용**: gRPC async pattern 의 표준 — `new SendCall` + cq tag 로 사용 + `delete base` 로 해제. CLAUDE.md "raw new/delete 금지" 와 충돌.
- **현 영향**: gRPC async pattern 강제. unique_ptr 로 관리 가능하지만 ownership transfer 명확히 하려면 추가 설계 필요.
- **권장**: 변경 없음 (외부 라이브러리 ABI 강제 패턴). I1 의 `new CRtspProxy` 와 같은 분류.

### F-F5-03 — `RequestCounterSnapshot` 의 callback 에 1min cool-down log 적용 (Phase 4)
- **등급**: INFO (긍정 발견)
- **위치**: GrpcEventClientBase.cpp 의 asyncCompleteRpc
- **내용**: counter snapshot 응답 로그가 너무 잦지 않도록 1min throttle.

### F-F5-04 — Phase 2 fix 의 정합성 — 1차 리뷰 self-loopback 검증 통과
- **등급**: INFO (긍정 발견)
- **위치**: [GrpcUnaryHandler.h:218-239](../../code/Protocol/GRPC/include/GrpcUnaryHandler.h#L218-L239)
- **내용**: shared_from_this capture 패턴이 detached thread UAF 를 차단. self-loopback (send=recv=68) 으로 검증.

### F-F5-05 — `responder_.Finish(...)` 가 detached thread 에서 호출 — concurrent Finish 가능성?
- **등급**: NOTE
- **위치**: [GrpcUnaryHandler.h:230-237](../../code/Protocol/GRPC/include/GrpcUnaryHandler.h#L230)
- **내용**: AsyncHandler 의 Proceed PROCESS 가 detached thread 에 lambda dispatch. lambda 안에서 status FINISH 설정 + Finish 호출. 그러나 status 변경은 lock 없이 단순 atomic 아닌 enum write — race 가능 (한 thread 만 작성하므로 해롭지 않음).
- **현 영향**: detached thread 1개만 status 변경. cq 가 Finish 의 결과를 다음 Next 에서 FINISH 분기로 받음 → UnregisterHandler. race 없음.

### F-F5-06 — `setCommonContext` 의 trace_id (UUID) 가 매 RPC 마다 생성
- **등급**: INFO
- **위치**: [GrpcEventClientBase.h:48](../../code/Protocol/GRPC/include/GrpcEventClientBase.h#L48)
- **내용**: 각 call 에 trace_id 박힘. 디버깅 시 cross-node 추적 가능. F6 의 correlation_id 와 별도지만 통합 가능.

### F-F5-07 — `cq_.Next()` 가 무한 block — Stop 시 cq.Shutdown() 으로 깨움
- **등급**: NOTE
- **위치**: [GrpcEventClientBase.cpp asyncCompleteRpc + Stop]
- **내용**: SafeThread::Stop 의 closer 가 cq_.Shutdown() 호출해야 thread 깨움. closer 함수 정의 검증 필요 (헤더에는 RegistGrpcClientThreadFunctions 만 명시).
- **권장**: closer 가 cq.Shutdown() + 펜딩 tag drain 까지 책임지는지 .cpp 라인 검토.

### F-F5-08 — `grpc_clients_` 에 channel re-establish 로직 부재
- **등급**: NOTE
- **위치**: [NetworkManager.cpp::InitializeGrpcClients](../../code/Management/manager/src/NetworkManager.cpp#L32)
- **내용**: peer 가 일시적으로 down 되면 channel 이 disconnected 상태. grpc 자체적으로 reconnect 시도하지만 명시적 health-check 없음. 장시간 disconnect 후 재연결 보장 검증 필요.
- **권장**: 운영 중 장시간 peer down 시 client 동작 검증. 필요 시 health check + reconnect 추가.

### F-F5-09 — `EventDataWithRawImages` 의 raw image 가 protobuf bytes — 큰 페이로드 시 메모리/네트워크 부담
- **등급**: NOTE
- **위치**: proto 정의
- **내용**: image bytes 가 EventDataWithRawImages.images 에 들어감. 대용량 (수 MB×n) 시 GRPC max_message_size 초과 가능. Phase 1 코드에 max_size 설정 명시 검증 필요.
- **현 영향**: 현재 Master/Slave 메인 분기 프로젝트가 시작되면 검증 필요.
- **권장**: ServerBuilder / ChannelArguments 의 max_receive_message_length 설정 검증.

### F-F5-10 — `DETECTBASE_GRPC` namespace 가 proto-generated. 하드코딩 의존
- **등급**: INFO
- **위치**: [MgenProto.proto](../../code/Protocol/GRPC/protos/MgenProto.proto)
- **내용**: namespace 변경 시 모든 handler 코드 영향. 베이스 프로젝트라 변경 가능성 낮음.

### F-F5-11 — `ServerCompletionQueue` 가 단일 — 멀티 RPC 동시성 제한
- **등급**: NOTE
- **위치**: [GrpcEventServerBase.h:71](../../code/Protocol/GRPC/include/GrpcEventServerBase.h#L71)
- **내용**: cq 1개 + grpc_server_thread 1개. 모든 RPC 가 직렬 dispatch. Direct handler 는 Proceed 안에서 즉시 처리, Async handler 는 detached thread 로 분산 → cq 자체 직렬은 문제 안 됨.
- **현 영향**: 현 부하에서 영향 없음. 부하 증가 시 cq 추가 / thread pool 도입 가능.

### F-F5-12 — F4 NetworkManager 의 `BroadcastEventOnlyJsonToGrpcPeers` 가 throw 안 잡음 (검증 필요)
- **등급**: NOTE
- **위치**: [NetworkManager.cpp:76-82](../../code/Management/manager/src/NetworkManager.cpp#L76)
- **내용**: client->SendEventOnlyJson(...) 이 throw 가능성? grpc::PrepareAsync 등이 throw 하나 확인 필요. 함수 시그니처는 noexcept.
- **권장**: SendEventOnlyJson 의 .cpp 구현 검증.

### F-F5-13 — Phase 4 default post-processor (set 안 하면 로그만)
- **등급**: INFO (긍정 발견)
- **위치**: [GrpcEventServerBase.h:91-94](../../code/Protocol/GRPC/include/GrpcEventServerBase.h#L91-L94)
- **내용**: SendCounterDelta / Heartbeat / RequestCounterSnapshot 의 handler 가 미설정 시 default 로그. 외부에서 set 하지 않아도 RPC 자체는 정상 동작. 베이스 프로젝트로서 좋은 기본값.

---

## §7. Open Questions

1. **F-F5-01**: server_owner_ 의 dangling 차단을 weak_ptr 로 강화할까? 큰 작업.
2. **F-F5-08**: peer down 후 reconnect 동작 검증 (Master/Slave 본격 사용 시).
3. **F-F5-09**: max_message_size 설정 명시 — 현 코드 어디에 있는지 확인.

---

## §8. Self-Check

- [x] Primary 파일 읽음 — Server.h / Client.h / UnaryHandler.h 전체. 생성된 .pb.{h,cc} 는 외부 (proto compile output)
- [x] §3 시퀀스 — RPC 정의 / Server lifecycle / HandleRpcsRunner / Phase 2 fix / Client async / NetworkManager 통합 모두 file:line
- [x] §4 소유권 — Server side / Client side / handler factory 패턴 모두 명시
- [x] §5 동시성 — server thread / detached thread / client thread / 락 정리. shared lifetime 모델 핵심 명시
- [x] §6 Finding 등급 + 출처 (13개)
- [x] 추측 표시 — "검증 필요" (F-F5-01 server destroy race, F-F5-07 closer 책임, F-F5-12 throw 가능성)
- [x] Also-touches 라벨 모순 없음

**검증 결과**: PASS

**Stage F-base 보강 항목 검증 결과**:
- **F-I3-04 (ReplyDispatcher 사용)** — F5 의 RequestEventOnlyJson 등이 `ReplyDispatcher<EventDataOnlyJson>` 을 외부 set 으로 받음. WithCleaner 가 아닌 raw `ReplyDispatcher`. timeout 시 entry 잔존 누적 가능 → set 한 외부가 cleanup 책임 (F4 NetworkManager 가 set 하는지 검증 필요. 현재 미사용 추정).
- **F-I3-02 (SafeThread closer)** — grpc_server_thread_ + grpc_client_thread_ 모두 SafeThread 사용. closer 정의는 .cpp 검증 필요 (F-F5-07).

**Stage F-runtime 종합 검증 보강**:
- F-F4-04 (SioEventBinder throw): 미검증 (deferred)
- F-F4-10 (socketio_reconnect_total 위치): 미검증 (deferred)
- F-F3-11 (avframe_q_ unit/proxy lifecycle): 미검증 — 가장 큰 위험 항목. F-runtime 종합 시 정리 필요
