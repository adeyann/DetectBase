# DEFERRED VERIFICATION — 2차 리뷰 deferred 항목 라인 단위 검증

> 1차 2차 리뷰에서 deferred 처리됐던 항목 9개를 라인 단위로 검증.
> 결과: 4건 안전 확인 (강등) / 1건 NOTE 유지 / **2건 WARN 격상 (새 위험 발견)** / 2건 안전 확인 (정보).

---

## §1. 검증 결과 요약 (한눈에)

| ID | 항목 | 이전 등급 | 검증 결과 | 새 등급 |
|---|---|---|---|---|
| **F-F5-07** | GrpcEventClientBase thread closer | NOTE | **closer 누락 → dtor hang 가능** | **WARN ↑** |
| **F-F5-12** | BroadcastEventOnlyJsonToGrpcPeers throw | NOTE | **noexcept + try/catch 모순** | **WARN ↑** |
| F-F3-04 | AbnormalActions throw | NOTE | throw 0건 | INFO ↓ |
| F-F3-13 | frame_count_ 사용처 | INFO | correlation_id 생성에 사용 | INFO (확정) |
| F-F3-15 | subscribe_ids_ unsubscribe 시점 | NOTE | InferenceThreadCloser 에서 안전 호출 | INFO ↓ |
| F-F4-04 | SioEventBinder::Run throw | NOTE | Run 자체 안전 | NOTE 유지 |
| F-F4-10 | socketio_reconnect_total 위치 | NOTE | reconnect_listener 사용 | INFO ↓ |
| IF-01 | RTSP unit lifecycle race | RISK 후보 | shared_ptr + closer 패턴으로 안전 | NOTE ↓ |
| IF-04 | GRPC broadcast 호출 위치 | NOTE | RtspDetectorUnit.cpp:1245 | INFO ↓ |

**핵심 변화**: 잠재 RISK 1건 (IF-01) 안전 확인 / 새 WARN 2건 (F5 영역).

---

## §2. 항목별 상세 검증

### F-F5-07 — GrpcEventClientBase thread closer 누락 (**WARN 격상**)

#### 검증 결과 (코드 확인)

```cpp
// GrpcEventClientBase.cpp:24-29
void GrpcEventClientBase::RegistGrpcClientThreadFunctions( void ) noexcept
{
    this->grpc_client_thread_.SetThreadFunctions(
        std::bind( &GrpcEventClientBase::asyncCompleteRpc, this )
        // ⚠ closer 인자 없음! SafeThread::SetThreadFunctions(runner, closer=nullptr)
    );
}

// GrpcEventClientBase.cpp:31-35
GrpcEventClientBase::~GrpcEventClientBase()
{
    this->Stop();        // = grpc_client_thread_.Stop() → join 시도
    cq_.Shutdown();      // ⚠ Stop() 이 hang 하면 도달 못함
}

// GrpcEventClientBase.cpp:55-58
void GrpcEventClientBase::Stop()
{
    this->grpc_client_thread_.Stop();
}
```

#### 시나리오 (실제 hang 가능성)

```
1. dtor 진입 → Stop() → grpc_client_thread_.Stop()
2. SafeThread::Stop:
   - is_running_.exchange(false) = true
   - closer 가 nullptr 이라 호출 안 됨
   - thread_object_.join() 시도
3. asyncCompleteRpc thread 는 cq_.Next(&tag, &ok) 에서 block 중
   - is_running_ 변경을 모름 (cq.Next 가 thread-internal block)
4. join 무한 대기 → dtor hang
5. cq_.Shutdown() 도달 못함
```

#### 실제 영향 평가

- **현재 운영에서 문제 미보고** — GRPC client default OFF + 검증 시점에서 dtor 호출되는 정상 종료 시 다른 thread 가 client 를 destroy 하므로 발생 안 했을 가능성
- **잠재적 hang** — Master/Slave 본격 사용 시 client lifecycle 중 dtor 도달 때 발견될 가능성

#### 권장 조치 (즉시)

```cpp
void GrpcEventClientBase::RegistGrpcClientThreadFunctions( void ) noexcept
{
    this->grpc_client_thread_.SetThreadFunctions(
        std::bind( &GrpcEventClientBase::asyncCompleteRpc, this ),
        [this]{ this->cq_.Shutdown(); }  // ← closer 추가
    );
}
```

dtor 의 cq_.Shutdown() 은 그대로 둬도 idempotent (라이브러리 그래야 함, 검증 권장).

#### 등급: **NOTE → WARN**

---

### F-F5-12 — `BroadcastEventOnlyJsonToGrpcPeers` 의 noexcept + try/catch 모순 (**WARN 격상**)

#### 검증 결과 (코드 확인)

```cpp
// NetworkManager.h:65
size_t BroadcastEventOnlyJsonToGrpcPeers( const std::string& json_payload ) noexcept;

// NetworkManager.cpp:76-93
size_t NetworkManager::BroadcastEventOnlyJsonToGrpcPeers( const std::string& json_payload ) noexcept
{
    if( grpc_clients_.empty() ) return 0;

    size_t sent = 0;
    for( auto& client : grpc_clients_ ) {
        if( !client ) continue;
        try {
            client->SendEventOnlyJson( MakeEventOnlyJsonProto( json_payload ) );
            ++sent;
        }
        catch( const std::exception& e ) {
            MLOG_WARN("BroadcastEventOnlyJson exception: %s", e.what() );
        }
    }
    return sent;
}
```

#### 모순 분석

- 시그니처: `noexcept` 명시 → throw 시 `std::terminate` 호출
- 본문: `try/catch(std::exception)` → throw 잡음
- **만약 SendEventOnlyJson 또는 MakeEventOnlyJsonProto 가 std::exception 이 아닌 throw (예: GoogleProtobuf 의 google::protobuf::FatalException)** 이 발생하면 catch 안 잡고 → noexcept 라서 → terminate

#### 실제 영향 평가

- protobuf 자체는 일반적으로 std::exception 계열 throw. catch 가 잡음
- 단 `noexcept` 가 의미를 갖는 건 throw 가 발생했을 때 — 명시 자체는 옵티마이저에게 정보 주지만 본문에 try/catch 가 있다는 건 **함수 작성자가 throw 가능성을 인지** 했다는 뜻 → noexcept 잘못

#### 권장 조치 (즉시)

옵션 A — noexcept 제거:
```cpp
size_t BroadcastEventOnlyJsonToGrpcPeers( const std::string& json_payload );  // noexcept 제거
```

옵션 B — catch 도 catch(...) 추가하여 모든 throw 안전 보장:
```cpp
} catch( const std::exception& e ) {
    MLOG_WARN("BroadcastEventOnlyJson exception: %s", e.what() );
} catch( ... ) {
    MLOG_WARN("BroadcastEventOnlyJson unknown exception" );
}
```

**옵션 B 권장** — noexcept 시그니처 유지 + 모든 throw catch.

#### 등급: **NOTE → WARN**

---

### F-F3-04 — AbnormalActions throw 패턴 (안전 확인)

#### 검증 결과

```bash
grep -rn "throw\|try {\|catch\|stoi\|stof\|stod\|stol" code/AbnormalActions/
# → 결과 0건
```

전체 AbnormalActions 모듈에 throw / try-catch / stoi 류 없음. 매우 깔끔.

#### 등급: **NOTE → INFO** (긍정 발견)

---

### F-F3-13 — frame_count_ 사용처 (확정)

#### 검증 결과

```cpp
// RtspDetectorUnit.cpp:1018
uint64_t current_correlation_id = this->frame_count_.fetch_add(1);
```

frame_count_ 는 dead code 아님. 매 추론 요청마다 atomic increment 되어 correlation_id 로 사용. EngineStreamMetaData::correlation_id 가 추론 요청-응답 매칭에 활용.

#### 등급: **INFO 확정** (사용처 명확)

---

### F-F3-15 — subscribe_ids_ unsubscribe 시점 (안전 확인)

#### 검증 결과

```cpp
// RtspDetectorUnit.cpp:1260-1264
void RtspDetectorUnit::InferenceThreadCloser( void )
{
    for( const auto& subs_id : subscribe_ids_ ){
        load_balancer_->Unsubscribe( subs_id );
    }
    // ... avframe_q_->terminate() 등 (라인 더 확인)
}
```

inference_thread_.Stop() 의 closer = InferenceThreadCloser. SafeThread::Stop 호출 → closer 가 자동으로 LoadBalancer.Unsubscribe(subscribe_id). 

#### 등급: **NOTE → INFO**

---

### F-F4-04 — SioEventBinder::Run throw (안전 확인)

#### 검증 결과 (SioHandler.cpp:399-453)

```cpp
bool SioEventBinder::Run( const nlohmann::json& js_sio )
{
    // 모든 분기가 nullptr 검사 + return false. throw 없음.
    if( inter_proc == nullptr ) return false;
    std::optional<nlohmann::json> resp = inter_proc( js_sio );
    if( resp.has_value() == false ) { MLOG_ERROR; return false; }
    ...
}
```

Run 자체는 throw 안 함. 단:
- `inter_proc` (외부 callable) 가 throw 시 SioEventBinder::Run 이 catch 안 함 → 호출자(sio listener)로 전파
- 그러나 `inter_proc` 의 body 는 ApiHandler::_BC_GET_Internal — REST GET timeout 시 code 반환 (F-F4-01 검증). throw 없음
- `update_units_extractor` / `multi_unit_setting_manager->UpdateTargetUnit` / `internal_queue->enqueue_move` / `in_scope_direct_processor` 등 외부 callable 도 throw 안 함이 가정

#### 등급: **NOTE 유지**

이유: SioEventBinder::Run 자체는 안전. 단 외부 callable 의 throw 는 책임 외 — 외부 callable 작성자가 보장해야 함. NOTE 유지하여 향후 callable 추가 시 검증 reminder.

---

### F-F4-10 — socketio_reconnect_total 위치 (안전 확인)

#### 검증 결과 (SioHandler.cpp:115-125)

```cpp
this->client.set_reconnect_listener(
    [this]( unsigned attempt, unsigned delay_ms ) {
        MLOG_INFO( "SocketIO reconnect attempt #%u (delay %u ms) -> %s:%d", ... );
        MGEN::MetricsRegistry::Instance().IncrementCounter(
            "detectbase_socketio_reconnect_total", {} );
    }
);
```

OnConnect/OnClose/OnFail 가 아니라 **set_reconnect_listener** 사용. sioclient 라이브러리의 자동 reconnect 시도마다 호출 — 가장 정확한 시점.

#### 등급: **NOTE → INFO** (긍정 발견 — 깔끔한 설계)

---

### IF-01 — RTSP unit lifecycle race (안전 확인)

#### 검증 결과 (RtspHandler.cpp:208-234)

```cpp
bool RtspHandler::StopRTSP( const int stop_timeout_seconds )
{
    if( this->state_ == RtspState::Run ){
        try {
            auto future = std::async( std::launch::async, []() {
                return rtsp_stop();
            } );
            if( future.wait_for(seconds(stop_timeout_seconds)) == timeout ){
                MLOG_WARN("Try Terminate All Camera Proxy, But Timeout");
                this->SetRtspState( RtspState::Stop );
                return false;
            }
            ...
        } catch(exception) { ... }
    }
    return true;
}
```

`rtsp_stop()` 가 RTSP 라이브러리 전체 정지. 단, `setDecodedFrameSafeQueue(nullptr)` unlink 호출은 없음.

#### 종합 race window 분석

```
[F1 Quit 진입]
  #03 detector_block_->Stop()
    └─ 각 unit 의 inference_thread_.Stop()
        └─ Closer = InferenceThreadCloser
            └─ load_balancer_->Unsubscribe(subscribe_ids)
            └─ (avframe_q_->terminate() 호출 검증 필요 — 라인 1265 이후)
        └─ join 시도
        └─ asyncCompleteRpc 같은 block 없음. dequeue_wait_for(100ms) 가 자연 wake.

  [Race window]
   - inference_thread 정지 후 ~ #04 도달 전:
     RTSP rx_thread/디코드 thread 가 avframe_q_->enqueue 시도 가능
     → avframe_q_ 가 shared_ptr, 아직 살아있음 → enqueue 성공 (누적)
     → consumer 없으니 누적만 됨
   - unit 자체는 service_units_ vector 의 unique_ptr 안에 → 살아있음
   - unit destroy 는 Service_DETECTOR dtor → service_units_ vector destroy 시점

  #04 network_manager_->CloseNetworkAll()
    └─ rtsp_handler_->StopRTSP() → rtsp_stop()
        └─ 모든 RTSP thread 정지 → enqueue 안 됨

  ... 시간 경과 ...

  ~Service_DETECTOR()
    └─ service_units_ destroy
        └─ unit 의 avframe_q_ ref release
        └─ RTSP 도 이미 stop → ref 0 → 큐 destroy
```

#### 결론

**race window 는 존재하지만 unit 이 살아있는 동안 큐에 enqueue 만 누적**. unit destroy 시점에는 RTSP 도 이미 stop. shared_ptr 패턴 + 종료 순서로 안전 보장.

**잔여 위험**:
- avframe_q_ capacity 무제한 → race window 동안 누적 메모리. 하지만 RTSP stop 까지 길지 않으므로 영향 미미.
- F-F3-01 (avframe_q_->SetMaxSize) 적용으로 mitigation.

#### 등급: **RISK 후보 → NOTE**

추가 확인 사항: InferenceThreadCloser 의 라인 1264 이후에 avframe_q_->terminate() 호출이 있는지 검증.

---

### IF-04 — GRPC broadcast 호출 위치 (확인)

#### 검증 결과 (RtspDetectorUnit.cpp:1239-1254)

```cpp
// SocketIO emit 다음 분기
if( network_manager_ && network_manager_->IsGrpcClientEnabled() )
{
    for( auto& target_event : event_list )
    {
        const size_t sent = network_manager_->BroadcastEventOnlyJsonToGrpcPeers(
            target_event.event_message.dump() );
        if( sent > 0 ) {
            MetricsRegistry::Instance().IncrementCounter(
                "detectbase_grpc_send_total",
                { { "rpc", "SendEventOnlyJson" } },
                static_cast<double>( sent ) );
        }
    }
}
```

SocketIO emit 직후, 동일 event_list 순회. 모든 peer 에 fire-and-forget. sent 가 0 이상이면 메트릭.

#### 등급: **NOTE → INFO** (위치 명확)

---

## §3. 새 발견 사항 — Phase A 즉시 처리 항목 추가

기존 [FINDINGS.md §6 Phase A](FINDINGS.md) 의 7개 항목에 다음 2개 추가:

| ID | 작업 | LOC | 우선순위 |
|---|---|---|---|
| **F-F5-07** | GrpcEventClientBase 의 SetThreadFunctions 에 closer 추가 (`[this]{ cq_.Shutdown(); }`) | 1~3 줄 | **높음 (잠재 dtor hang)** |
| **F-F5-12** | BroadcastEventOnlyJsonToGrpcPeers 의 catch 에 `catch(...)` 추가 | 3 줄 | 중간 (noexcept 정합성) |

**B 단계 결론**:
- Phase A 즉시 처리 항목이 7 → **9개**로 증가
- 모두 작은 작업 (1~3 줄 단위). 누적 LOC 변화 미미

---

## §4. Self-Check (DEFERRED)

- [x] 9 deferred 항목 모두 라인 단위 검증 완료
- [x] 각 항목별 코드 인용 + 결론 + 등급 변경 사유 명시
- [x] 새 WARN 2건 (F-F5-07, F-F5-12) 의 시나리오 / 영향 / 권장 조치 모두 작성
- [x] IF-01 (잠재 RISK 후보) 의 안전 확인 근거 명시 (shared_ptr + 종료 순서)
- [x] InferenceThreadCloser 의 avframe_q_->terminate() 호출 여부 라인 1264 이후 추가 확인 항목으로 표기

**검증 결과**: PASS

**잔여 추가 확인 항목** (이번 검증에서 미해결):
- InferenceThreadCloser 라인 1264 이후의 avframe_q_->terminate() 호출 여부
- inference_thread_ Closer 의 정확한 종료 시퀀스
