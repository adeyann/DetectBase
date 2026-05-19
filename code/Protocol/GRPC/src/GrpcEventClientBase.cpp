#include "GrpcEventClientBase.h"
#include "UUIDGenerator.h"
#include "MgenLogger.h"
#include "MetricsRegistry.h"

#include <chrono>

namespace MGEN
{
    GrpcEventClientBase::GrpcEventClientBase( std::shared_ptr<grpc::Channel> channel )
        : channel_ ( std::move( channel ) )
        , stub_    ( DETECTBASE_GRPC::NewStub( channel_ ) )
    {
        this->RegistGrpcClientThreadFunctions();
    }

    GrpcEventClientBase::GrpcEventClientBase( const std::string& ip, const int port )
        : channel_ ( GrpcEventClientBase::MakeChannel( ip, port ) )
        , stub_    ( DETECTBASE_GRPC::NewStub(channel_) )
    {
        this->RegistGrpcClientThreadFunctions();
    }

    void GrpcEventClientBase::RegistGrpcClientThreadFunctions( void ) noexcept
    {
        this->grpc_client_thread_.SetThreadFunctions(
            std::bind( &GrpcEventClientBase::asyncCompleteRpc, this ),
            // closer: cq_.Shutdown() 으로 asyncCompleteRpc 의 cq_.Next() block 해제 → join 정상 진행.
            // closer 누락 시 dtor → Stop → join 무한 대기 → cq_.Shutdown 도달 못함 (잠재 hang).
            [this]{ this->cq_.Shutdown(); }
        );
    }

    GrpcEventClientBase::~GrpcEventClientBase()
    {
        // Stop() 안의 SafeThread::Stop 이 closer 를 통해 cq_.Shutdown() 호출.
        // 따라서 dtor 의 명시적 cq_.Shutdown() 중복 호출 불필요.
        this->Stop();
    }

    std::shared_ptr<grpc::Channel> GrpcEventClientBase::MakeChannel( const std::string& ip, const int port ) const noexcept
    {
        grpc::ChannelArguments args;
        args.SetInt( GRPC_ARG_MAX_RECONNECT_BACKOFF_MS,     2000 );
        args.SetInt( GRPC_ARG_INITIAL_RECONNECT_BACKOFF_MS, 1000 );

        std::string url { "" };
        url += ip + ":" + std::to_string( port );

        return grpc::CreateCustomChannel( url, grpc::InsecureChannelCredentials(), args );
    }

    bool GrpcEventClientBase::Start()
    {
        this->grpc_client_thread_.Start();
        return true;
    }

    void GrpcEventClientBase::Stop()
    {
        this->grpc_client_thread_.Stop();
    }

    void GrpcEventClientBase::setCommonContext( BaseCall& call, const std::string& uuid ) const noexcept
    {
        call.trace_id   = uuid;
        call.start_time = std::chrono::steady_clock::now();

        call.context.AddMetadata( "trace-id", call.trace_id );
        call.context.set_deadline( std::chrono::system_clock::now() + std::chrono::seconds(5) );
    }

    // -------------------- Fire-and-forget --------------------

    bool GrpcEventClientBase::SendEventOnlyJson( const EventDataOnlyJson&& event_data )
    {
        const auto& uuid = event_data.uuid();
        if( UUID::isValidUUID( uuid ) == false )
            return false;

        auto call = new SendCall();
        setCommonContext(*call, uuid );

        call->response_reader = stub_->PrepareAsyncSendEventOnlyJson(&call->context, event_data, &cq_);
        call->response_reader->StartCall();
        call->response_reader->Finish(&call->reply, &call->status, (void*)call);

        return true;
    }

    bool GrpcEventClientBase::SendEventWithImages( const EventDataWithRawImages&& event_data )
    {
        const auto& uuid = event_data.uuid();
        if( UUID::isValidUUID( uuid ) == false )
            return false;

        auto call = new SendCall();
        setCommonContext(*call, uuid );

        call->response_reader = stub_->PrepareAsyncSendEventWithImages(&call->context, event_data, &cq_);
        call->response_reader->StartCall();
        call->response_reader->Finish(&call->reply, &call->status, (void*)call);

        return true;
    }

    // -------------------- Request with response --------------------

    bool GrpcEventClientBase::RequestEventOnlyJson( const EventDataOnlyJson&& event_data, std::function<void(const std::string&)> on_result )
    {
        const auto& uuid = event_data.uuid();
        if( UUID::isValidUUID( uuid ) == false )
            return false;

        auto call = new JsonResponseCall();
        setCommonContext( *call, uuid );
        call->callback = std::move(on_result);

        call->response_reader = stub_->PrepareAsyncRequestEventOnlyJson(&call->context, event_data, &cq_);
        call->response_reader->StartCall();
        call->response_reader->Finish(&call->response, &call->status, (void*)call);

        return true;
    }

    bool GrpcEventClientBase::RequestEventWithImages( const EventDataWithRawImages&& event_data, std::function<void(const std::string&)> on_result )
    {
        const auto& uuid = event_data.uuid();
        if( UUID::isValidUUID( uuid ) == false )
            return false;

        auto call = new JsonResponseCall();
        setCommonContext( *call, uuid );
        call->callback = std::move(on_result);

        call->response_reader = stub_->PrepareAsyncRequestEventWithImages(&call->context, event_data, &cq_);
        call->response_reader->StartCall();
        call->response_reader->Finish(&call->response, &call->status, (void*)call);

        return true;
    }

    // -------------------- [Phase 4 샘플] Counter / Heartbeat --------------------

    bool GrpcEventClientBase::SendCounterDelta( const CounterDelta&& delta )
    {
        // UUID 없는 메시지 → 매 호출 새 trace_id 생성.
        const auto trace_id = UUID::GetGenerator()->generate();

        auto call = new SendCall();
        setCommonContext( *call, trace_id );

        call->response_reader = stub_->PrepareAsyncSendCounterDelta( &call->context, delta, &cq_ );
        call->response_reader->StartCall();
        call->response_reader->Finish( &call->reply, &call->status, (void*)call );
        return true;
    }

    bool GrpcEventClientBase::SendHeartbeat( const HeartbeatPing&& ping )
    {
        const auto trace_id = UUID::GetGenerator()->generate();

        auto call = new SendCall();
        setCommonContext( *call, trace_id );

        call->response_reader = stub_->PrepareAsyncSendHeartbeat( &call->context, ping, &cq_ );
        call->response_reader->StartCall();
        call->response_reader->Finish( &call->reply, &call->status, (void*)call );
        return true;
    }

    bool GrpcEventClientBase::RequestCounterSnapshot( const CounterRequest&& req,
                                                     std::function<void(const CounterSnapshot&)> on_result )
    {
        const auto trace_id = UUID::GetGenerator()->generate();

        auto call = new CounterSnapshotCall();
        setCommonContext( *call, trace_id );
        call->callback = std::move( on_result );

        call->response_reader = stub_->PrepareAsyncRequestCounterSnapshot( &call->context, req, &cq_ );
        call->response_reader->StartCall();
        call->response_reader->Finish( &call->response, &call->status, (void*)call );
        return true;
    }

    // -------------------- Completion Queue --------------------

    void GrpcEventClientBase::asyncCompleteRpc()
    {
        void* got_tag = nullptr;
        bool  ok      = false;

        // Phase 6: GRPC FAIL 로그 cool-down — 같은 status code 가 1분 내 반복되면 첫 번째만 출력.
        // (그 사이 메트릭 (send_failed_total) 은 누적되어 가시화됨.)
        std::chrono::steady_clock::time_point last_fail_log {};

        auto&  running = this->grpc_client_thread_.GetRunningFlag();
        while( running.load() == true ){
            auto deadline = std::chrono::system_clock::now() + std::chrono::milliseconds(100);
            if( cq_.AsyncNext(&got_tag, &ok, deadline) != grpc::CompletionQueue::GOT_EVENT ){
                continue;
            }

            if( !got_tag )
                continue;

            auto* base     = static_cast<BaseCall*>(got_tag);
            auto  duration = std::chrono::steady_clock::now() - base->start_time;
            auto  ms       = std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();

            if( auto* call = dynamic_cast<SendCall*>(base) ){
                if( call->status.ok() ){
                    // 성공 로그는 INFO — 요청량 많으면 spam 가능. DEBUG 로 낮추거나 제거 검토.
                    MLOG_DEBUG( "[GRPC OK] trace_id=%s duration=%ldms",
                        call->trace_id.c_str(), ms );
                    MGEN::MetricsRegistry::Instance().IncrementCounter(
                        "detectbase_grpc_send_success_total", { { "rpc", "Send" } } );
                }
                else {
                    MGEN::MetricsRegistry::Instance().IncrementCounter(
                        "detectbase_grpc_send_failed_total",
                        { { "rpc", "Send" }, { "code", std::to_string( call->status.error_code() ) } } );

                    const auto now = std::chrono::steady_clock::now();
                    if( now - last_fail_log > std::chrono::minutes( 1 ) ) {
                        MLOG_WARN( "[GRPC FAIL] trace_id=%s code=%d msg=%s (cool-down 1min until next log)",
                            call->trace_id.c_str(), call->status.error_code(), call->status.error_message().c_str() );
                        last_fail_log = now;
                    }
                }
                delete call;
            }
            else if( auto* call = dynamic_cast<JsonResponseCall*>(base) ){
                if( call->status.ok() ){
                    MLOG_INFO( "[GRPC OK][Response] trace_id=%s duration=%ldms",
                        call->trace_id.c_str(), ms );

                    if( call->callback ){
                        call->callback( call->response.json_data() );
                    }
                }
                else {
                    MLOG_ERROR( "[GRPC FAIL][Response] trace_id=%s code=%d msg=%s",
                        call->trace_id.c_str(), call->status.error_code(), call->status.error_message().c_str() );
                }
                delete call;
            }
            else if( auto* call = dynamic_cast<CounterSnapshotCall*>(base) ){
                // [Phase 4 샘플] CounterSnapshot 응답.
                if( call->status.ok() ){
                    MLOG_INFO( "[GRPC OK][CounterSnapshot] trace_id=%s duration=%ldms",
                        call->trace_id.c_str(), ms );
                    if( call->callback ){
                        call->callback( call->response );
                    }
                }
                else {
                    MLOG_ERROR( "[GRPC FAIL][CounterSnapshot] trace_id=%s code=%d msg=%s",
                        call->trace_id.c_str(), call->status.error_code(), call->status.error_message().c_str() );
                }
                delete call;
            }
            else {
                MLOG_ERROR("[GRPC] Unknown tag received");
            }
        }
    }

}
