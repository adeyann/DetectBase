#include "GrpcEventServerBase.h"
#include "GrpcUnaryHandler.h"
#include "MgenLogger.h"
#include "json/json.hpp"

using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::ServerAsyncResponseWriter;
using grpc::Status;

namespace MGEN
{
    // -------------------- GrpcEventServerBase Core --------------------

    GrpcEventServerBase::GrpcEventServerBase( const std::string& server_ip, const int server_port )
        : server_address_ ( GrpcEventServerBase::MakeUrl( server_ip, server_port ) )
    {
        this->RegistGrpcClientThreadFunctions();
    }

    GrpcEventServerBase::~GrpcEventServerBase()
    {
        this->Stop();
    }

    void GrpcEventServerBase::RegistGrpcClientThreadFunctions( void ) noexcept
    {
        this->grpc_server_thread_.SetThreadFunctions(
            // runner
            std::bind( &GrpcEventServerBase::HandleRpcsRunner, this ),
            // closer
            std::bind( &GrpcEventServerBase::HandleRpcsCloser, this )
        );
    }

    std::string GrpcEventServerBase::MakeUrl( const std::string& ip, const int port ) const noexcept
    {
        std::string url { "" };
        url += ip + ":" + std::to_string( port );
        return url;
    }

    void GrpcEventServerBase::DrainCompletionQueue()
    {
        void* tag;
        bool  ok;

        while( cq_->Next(&tag, &ok) ){
            static_cast<CallDataBase*>( tag )->Proceed( ok );
        }
    }

    // -- Phase 2: handler registry -----------------------------------

    void GrpcEventServerBase::RegisterHandler( std::shared_ptr<CallDataBase> handler )
    {
        if( !handler ) return;
        std::lock_guard<std::mutex> lck { alive_mtx_ };
        alive_handlers_[ handler.get() ] = std::move( handler );
    }

    void GrpcEventServerBase::UnregisterHandler( CallDataBase* raw_handler )
    {
        if( !raw_handler ) return;
        std::shared_ptr<CallDataBase> released; // map 밖에서 release (lock 외부 dtor)
        {
            std::lock_guard<std::mutex> lck { alive_mtx_ };
            auto it = alive_handlers_.find( raw_handler );
            if( it != alive_handlers_.end() ) {
                released = std::move( it->second );
                alive_handlers_.erase( it );
            }
        }
        // released 가 dtor 호출. 다른 곳 (detached thread) 에서 capture 했다면 ref 유지.
    }

    void GrpcEventServerBase::ClearAllHandlers( void )
    {
        std::unordered_map<void*, std::shared_ptr<CallDataBase>> released;
        {
            std::lock_guard<std::mutex> lck { alive_mtx_ };
            released.swap( alive_handlers_ );
        }
        // released 가 함수 종료 시 정리. detached thread 의 capture 만 남음 (thread 종료 시 자동 dtor).
    }

    void GrpcEventServerBase::Run()
    {
        ServerBuilder builder;
        builder.AddListeningPort( server_address_, grpc::InsecureServerCredentials() );
        builder.RegisterService ( &service_ );

        cq_     = builder.AddCompletionQueue();
        server_ = builder.BuildAndStart();
        MLOG_INFO("[GRPC SERVER] Listening on %s", server_address_.c_str());

        // --- Register Handlers (Phase 2: shared_ptr factory 사용) ---

        // 1. Fire-and-forget: SendEventOnlyJson
        GrpcUnaryDirectProcessHandler<EventDataOnlyJson, Empty>::CreateAndRegister(
            this->weak_from_this(), &service_, cq_.get(),
            [this]( ServerContext* ctx, EventDataOnlyJson* req,
                    ServerAsyncResponseWriter<Empty>* responder,
                    grpc::CompletionQueue* cq1, grpc::ServerCompletionQueue* cq2, void* tag ){
                service_.RequestSendEventOnlyJson( ctx, req, responder, cq1, cq2, tag );
            },
            queue_send_event_json_,
            send_event_json_post_process_
        );

        // 2. Fire-and-forget: SendEventWithImages
        GrpcUnaryDirectProcessHandler<EventDataWithRawImages, Empty>::CreateAndRegister(
            this->weak_from_this(), &service_, cq_.get(),
            [this]( ServerContext* ctx, EventDataWithRawImages* req,
                    ServerAsyncResponseWriter<Empty>* responder,
                    grpc::CompletionQueue* cq1, grpc::ServerCompletionQueue* cq2, void* tag ){
                service_.RequestSendEventWithImages( ctx, req, responder, cq1, cq2, tag );
            },
            queue_send_event_images_,
            send_event_images_post_process_
        );

        // 3. Request/Reply: RequestEventOnlyJson
        GrpcUnaryAsyncProcessHandler<EventDataOnlyJson, EventDataOnlyJson>::CreateAndRegister(
            this->weak_from_this(), &service_, cq_.get(),
            [this]( ServerContext* ctx, EventDataOnlyJson* req,
                    ServerAsyncResponseWriter<EventDataOnlyJson>* responder,
                    grpc::CompletionQueue* cq1, grpc::ServerCompletionQueue* cq2, void* tag ){
                service_.RequestRequestEventOnlyJson( ctx, req, responder, cq1, cq2, tag );
            },
            queue_request_event_json_in_,
            dispatcher_event_json_,
            []( const EventDataOnlyJson& req ){ return req.uuid(); },
            3000
        );

        // 4. Request/Reply: RequestEventWithImages
        GrpcUnaryAsyncProcessHandler<EventDataWithRawImages, EventDataOnlyJson>::CreateAndRegister(
            this->weak_from_this(), &service_, cq_.get(),
            [this]( ServerContext* ctx, EventDataWithRawImages* req,
                    ServerAsyncResponseWriter<EventDataOnlyJson>* responder,
                    grpc::CompletionQueue* cq1, grpc::ServerCompletionQueue* cq2, void* tag ){
                service_.RequestRequestEventWithImages( ctx, req, responder, cq1, cq2, tag );
            },
            queue_request_event_images_in_,
            dispatcher_event_images_,
            []( const EventDataWithRawImages& req ){ return req.uuid(); },
            3000
        );

        // [Phase 4 샘플] 5. Fire-and-forget: SendCounterDelta
        GrpcUnaryDirectProcessHandler<CounterDelta, Empty>::CreateAndRegister(
            this->weak_from_this(), &service_, cq_.get(),
            [this]( ServerContext* ctx, CounterDelta* req,
                    ServerAsyncResponseWriter<Empty>* responder,
                    grpc::CompletionQueue* cq1, grpc::ServerCompletionQueue* cq2, void* tag ){
                service_.RequestSendCounterDelta( ctx, req, responder, cq1, cq2, tag );
            },
            nullptr,  // queue 없음 (직접 handler 가 처리)
            send_counter_delta_post_process_
        );

        // [Phase 4 샘플] 6. Fire-and-forget: SendHeartbeat
        GrpcUnaryDirectProcessHandler<HeartbeatPing, Empty>::CreateAndRegister(
            this->weak_from_this(), &service_, cq_.get(),
            [this]( ServerContext* ctx, HeartbeatPing* req,
                    ServerAsyncResponseWriter<Empty>* responder,
                    grpc::CompletionQueue* cq1, grpc::ServerCompletionQueue* cq2, void* tag ){
                service_.RequestSendHeartbeat( ctx, req, responder, cq1, cq2, tag );
            },
            nullptr,
            send_heartbeat_post_process_
        );

        // [Phase 4 샘플] 7. Request-Response: RequestCounterSnapshot
        // DirectProcessHandler 의 PostProcessHandler 가 reply (CounterSnapshot) 를 채움.
        // (UUID 기반 dispatcher 패턴 안 씀 — 응답이 즉시 가능한 simple query).
        GrpcUnaryDirectProcessHandler<CounterRequest, CounterSnapshot>::CreateAndRegister(
            this->weak_from_this(), &service_, cq_.get(),
            [this]( ServerContext* ctx, CounterRequest* req,
                    ServerAsyncResponseWriter<CounterSnapshot>* responder,
                    grpc::CompletionQueue* cq1, grpc::ServerCompletionQueue* cq2, void* tag ){
                service_.RequestRequestCounterSnapshot( ctx, req, responder, cq1, cq2, tag );
            },
            nullptr,
            request_counter_snapshot_handler_
        );

        this->grpc_server_thread_.Start();
    }

    void GrpcEventServerBase::Stop()
    {
        // Run() 호출 전 Stop() 시 server_ 가 nullptr 일 수 있음
        if( server_ != nullptr ){
            server_->Shutdown();
        }
        this->grpc_server_thread_.Stop();

        // Phase 2: 모든 handler shared_ptr 강제 cleanup.
        // detached thread 가 capture 한 self 만 남으면 thread 종료 시 자동 dtor.
        this->ClearAllHandlers();
    }

    void GrpcEventServerBase::HandleRpcsRunner()
    {
        void* tag;
        bool  ok;

        auto&  running = this->grpc_server_thread_.GetRunningFlag();
        while( running.load() == true ){
            if( cq_->Next(&tag, &ok) ){
                static_cast<CallDataBase*>(tag)->Proceed(ok);
            }
        }
    }

    void GrpcEventServerBase::HandleRpcsCloser()
    {
        this->cq_->Shutdown();
        this->DrainCompletionQueue();
    }

    // -------------------- Queue Setters --------------------

    void GrpcEventServerBase::SetSendEventOnlyJsonQueue(std::shared_ptr<SafeQueue<EventDataOnlyJson>> q)
    {
        queue_send_event_json_ = q;
    }

    void GrpcEventServerBase::SetSendEventWithImagesQueue(std::shared_ptr<SafeQueue<EventDataWithRawImages>> q)
    {
        queue_send_event_images_ = q;
    }

    void GrpcEventServerBase::SetRequestEventOnlyJsonQueues(
        std::shared_ptr<SafeQueue<EventDataOnlyJson>> in,
        std::shared_ptr<ReplyDispatcher<EventDataOnlyJson>> dispatcher)
    {
        queue_request_event_json_in_ = in;
        dispatcher_event_json_ = dispatcher;
    }

    void GrpcEventServerBase::SetRequestEventWithImagesQueues(
        std::shared_ptr<SafeQueue<EventDataWithRawImages>> in,
        std::shared_ptr<ReplyDispatcher<EventDataOnlyJson>> dispatcher)
    {
        queue_request_event_images_in_ = in;
        dispatcher_event_images_ = dispatcher;
    }

    void GrpcEventServerBase::SetSendEventOnlyJsonPostProcesser( std::function<void(const EventDataOnlyJson&, Empty&)> post_processor )
    {
        send_event_json_post_process_ = post_processor;
    }

    void GrpcEventServerBase::SetSendEventWithImagesPostProcesser( std::function<void(const EventDataWithRawImages&, Empty&)> post_processor )
    {
        send_event_images_post_process_ = post_processor;
    }

    // [Phase 4 샘플] post-processor setter.
    void GrpcEventServerBase::SetSendCounterDeltaPostProcesser( std::function<void(const CounterDelta&, Empty&)> post )
    {
        send_counter_delta_post_process_ = post;
    }

    void GrpcEventServerBase::SetSendHeartbeatPostProcesser( std::function<void(const HeartbeatPing&, Empty&)> post )
    {
        send_heartbeat_post_process_ = post;
    }

    void GrpcEventServerBase::SetRequestCounterSnapshotHandler( std::function<void(const CounterRequest&, CounterSnapshot&)> post )
    {
        request_counter_snapshot_handler_ = post;
    }

} // namespace MGEN
