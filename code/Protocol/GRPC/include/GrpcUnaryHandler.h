#pragma once

#include "GrpcEventServerBase.h"
#include "UUIDGenerator.h"

#include <grpcpp/grpcpp.h>
#include <string>
#include <thread>
#include <functional>

namespace MGEN
{
    using grpc::ServerContext;
    using grpc::ServerAsyncResponseWriter;
    using grpc::Status;
    using grpc::StatusCode;

    // ============================================================================
    // Phase 2 (분석 6 fix) — shared_ptr lifetime model
    //
    // 핵심:
    //  - 모든 handler 는 std::make_shared 로 생성 + Server::RegisterHandler 로 등록
    //  - handler 의 Proceed 는 'delete this' 대신 server->UnregisterHandler(this) 호출
    //  - detached thread 가 shared_from_this() capture → thread 살아있는 한 객체 살아있음
    //  - server stop 시 ClearAllHandlers() — detached thread 의 capture 만 남으면 자동 정리
    //
    // 사용:
    //   auto h = MGEN::CreateAndRegisterUnaryDirectHandler<EventDataOnlyJson, Empty>(
    //              server_owner, &service_, cq, request_func, queue, post_process);
    //   // h 는 shared_ptr<...>. 더 이상 외부에서 잡고 있을 필요 없음 (server 가 잡음).
    // ============================================================================

    template<typename RequestType, typename ResponseType>
    class GrpcUnaryHandlerBase : public GrpcEventServerBase::CallDataBase
    {
    public:
        using RequestFunc
            = std::function<void(
                ServerContext*, RequestType*, ServerAsyncResponseWriter<ResponseType>*,
                grpc::CompletionQueue*, grpc::ServerCompletionQueue*, void*)>;

    protected:
        // W-13 fix: server 인자 raw* → weak_ptr. handler 가 server destroy 후 dangling 차단.
        GrpcUnaryHandlerBase( std::weak_ptr<GrpcEventServerBase> server,
                              DETECTBASE_GRPC::AsyncService* service,
                              grpc::ServerCompletionQueue* cq,
                              RequestFunc request_func )
            : server_owner_( std::move( server ) )
            , service_     ( service )
            , cq_          ( cq )
            , request_func_( request_func )
            , responder_   ( &ctx_ )
            , status_      ( CREATE )
        {
            //
        }

        // shared_ptr 생성 직후 외부에서 호출. ctor 에서 shared_from_this() 호출 불가 라서 분리.
    public:
        void Init() { this->Proceed( true ); }

        // W-13 helper: server_owner_ lock 후 method 호출. server destroy 됐으면 no-op.
        // FINISH 단계의 UnregisterHandler 자가 호출에서 사용.
        void TryUnregister() noexcept {
            if( auto srv = server_owner_.lock() ) {
                srv->UnregisterHandler( this );
            }
            // server 이미 destroy → ClearAllHandlers 가 이미 정리. no-op 안전.
        }

    protected:
        // W-13: weak_ptr 로 server 보유 → dangling 차단
        std::weak_ptr<GrpcEventServerBase> server_owner_;
        DETECTBASE_GRPC::AsyncService* service_;
        grpc::ServerCompletionQueue*   cq_;
        grpc::ServerContext            ctx_;
        RequestType                    request_;
        ResponseType                   reply_;
        RequestFunc                    request_func_;
        grpc::ServerAsyncResponseWriter<ResponseType> responder_;

        enum CallStatus { CREATE, PROCESS, FINISH };
        CallStatus status_;
    };

    // -- DirectProcessHandler (fire-and-forget) --------------------------

    template<typename RequestType, typename ResponseType>
    class GrpcUnaryDirectProcessHandler : public GrpcUnaryHandlerBase<RequestType, ResponseType>
    {
        using Base = GrpcUnaryHandlerBase<RequestType, ResponseType>;
    public:
        using PostProcessHandler = std::function<void(const RequestType&, ResponseType&)>;

        // W-13: server 인자 weak_ptr 로 변경
        GrpcUnaryDirectProcessHandler( std::weak_ptr<GrpcEventServerBase> server,
                                       DETECTBASE_GRPC::AsyncService* service,
                                       grpc::ServerCompletionQueue* cq,
                                       typename Base::RequestFunc request_func,
                                       std::shared_ptr<SafeQueue<RequestType>> in_queue,
                                       PostProcessHandler handler = nullptr )
            : Base( std::move( server ), service, cq, request_func )
            , in_queue_( in_queue )
            , handler_ ( handler )
        {
            // 주의: ctor 에서 Proceed 호출 안 함. factory 가 make_shared → Register → Init 순서로.
        }

        void Proceed( bool ok ) override final
        {
            if( this->status_ == Base::CREATE ) {
                this->status_ = Base::PROCESS;
                this->request_func_( &this->ctx_, &this->request_, &this->responder_,
                                     this->cq_, this->cq_, this );
            }
            else if( this->status_ == Base::PROCESS ) {
                if( !ok ) {
                    // 종료 시그널. 자가 등록 해제 → 자동 dtor.
                    this->TryUnregister();
                    return;
                }

                // 다음 요청 받을 새 handler 등록.
                CreateAndRegister( this->server_owner_, this->service_, this->cq_,
                                   this->request_func_, in_queue_, handler_ );

                if( in_queue_ ) {
                    if( handler_ ) {
                        in_queue_->enqueue_copy( this->request_ );
                    } else {
                        in_queue_->enqueue_move( std::move( this->request_ ) );
                    }
                }

                if( handler_ ) {
                    handler_( this->request_, this->reply_ );
                }

                this->status_ = Base::FINISH;
                this->responder_.Finish( this->reply_, Status::OK, this );
            }
            else { // FINISH
                this->TryUnregister();
            }
        }

        // factory: make_shared + Register + Init.
        // W-13: server 인자 weak_ptr.
        static std::shared_ptr<GrpcUnaryDirectProcessHandler> CreateAndRegister(
            std::weak_ptr<GrpcEventServerBase> server,
            DETECTBASE_GRPC::AsyncService* service,
            grpc::ServerCompletionQueue* cq,
            typename Base::RequestFunc request_func,
            std::shared_ptr<SafeQueue<RequestType>> in_queue,
            PostProcessHandler handler = nullptr )
        {
            // server 가 destroy 됐으면 등록 불가 → no-op (nullptr 반환)
            auto srv = server.lock();
            if( !srv ) return nullptr;

            auto h = std::make_shared<GrpcUnaryDirectProcessHandler>(
                server, service, cq, request_func, in_queue, handler );
            srv->RegisterHandler( h );
            h->Init();
            return h;
        }

    private:
        std::shared_ptr<SafeQueue<RequestType>> in_queue_;
        PostProcessHandler                       handler_;
    };

    // -- AsyncProcessHandler (request-response, detached thread 사용) ----

    template<typename RequestType, typename ResponseType>
    class GrpcUnaryAsyncProcessHandler : public GrpcUnaryHandlerBase<RequestType, ResponseType>
    {
        using Base = GrpcUnaryHandlerBase<RequestType, ResponseType>;
    public:
        using ExtractUUIDFunc = std::function<std::string(const RequestType&)>;

        // W-13: server 인자 weak_ptr 로 변경
        GrpcUnaryAsyncProcessHandler( std::weak_ptr<GrpcEventServerBase> server,
                                      DETECTBASE_GRPC::AsyncService* service,
                                      grpc::ServerCompletionQueue* cq,
                                      typename Base::RequestFunc request_func,
                                      std::shared_ptr<SafeQueue<RequestType>> in_queue,
                                      std::shared_ptr<ReplyDispatcher<ResponseType>> dispatcher,
                                      ExtractUUIDFunc uuid_extractor,
                                      unsigned int timeout_ms = 3000 )
            : Base( std::move( server ), service, cq, request_func )
            , in_queue_      ( in_queue )
            , dispatcher_    ( dispatcher )
            , uuid_extractor_( uuid_extractor )
            , timeout_ms_    ( timeout_ms )
        {
            // 주의: ctor 에서 Proceed 호출 안 함.
        }

        void Proceed( bool ok ) override final
        {
            if( this->status_ == Base::CREATE ) {
                this->status_ = Base::PROCESS;
                this->request_func_( &this->ctx_, &this->request_, &this->responder_,
                                     this->cq_, this->cq_, this );
            }
            else if( this->status_ == Base::PROCESS ) {
                if( !ok ) {
                    this->TryUnregister();
                    return;
                }

                // 다음 요청 받을 새 handler 등록.
                CreateAndRegister( this->server_owner_, this->service_, this->cq_,
                                   this->request_func_, in_queue_, dispatcher_,
                                   uuid_extractor_, timeout_ms_ );

                const auto uuid = uuid_extractor_( this->request_ );
                if( uuid.empty() ) {
                    const auto error_status = Status( StatusCode::INVALID_ARGUMENT, "UUID missing" );
                    this->status_ = Base::FINISH;
                    this->responder_.Finish( ResponseType{}, error_status, this );
                    return;
                }
                if( UUID::isValidUUID( uuid ) == false ) {
                    const auto error_status = Status( StatusCode::INVALID_ARGUMENT, "UUID invalid" );
                    this->status_ = Base::FINISH;
                    this->responder_.Finish( ResponseType{}, error_status, this );
                    return;
                }

                if( in_queue_ ) {
                    in_queue_->enqueue_move( std::move( this->request_ ) );
                }

                // [Phase 2 fix] shared_from_this() capture → thread 살아있는 한 객체 살아있음.
                // server stop 시 alive_handlers_ 가 clear 되지만 self capture 가 ref 유지.
                // wait_and_get 끝난 후 lambda scope 종료 → ref -1 → 자동 dtor (그 시점에 server 도 정리됨).
                auto self = std::static_pointer_cast<GrpcUnaryAsyncProcessHandler>(
                    this->shared_from_this() );

                std::thread( [ self,
                               uuid,
                               dispatcher_inst = this->dispatcher_,
                               timeout         = this->timeout_ms_ ] {
                    auto result = dispatcher_inst->wait_and_get( uuid, timeout );

                    self->status_ = Base::FINISH;

                    if( result.has_value() ) {
                        self->responder_.Finish( *result, Status::OK, self.get() );
                    } else {
                        const auto error_status = Status( StatusCode::DEADLINE_EXCEEDED, "Timeout" );
                        self->responder_.Finish( ResponseType{}, error_status, self.get() );
                    }
                    // self 해제 시점 = lambda scope 종료. 외부 alive_handlers_ 가 이미 erase 됐으면 dtor 발동.
                }).detach();
            }
            else { // FINISH
                this->TryUnregister();
            }
        }

        // factory: make_shared + Register + Init.
        // W-13: server 인자 weak_ptr.
        static std::shared_ptr<GrpcUnaryAsyncProcessHandler> CreateAndRegister(
            std::weak_ptr<GrpcEventServerBase> server,
            DETECTBASE_GRPC::AsyncService* service,
            grpc::ServerCompletionQueue* cq,
            typename Base::RequestFunc request_func,
            std::shared_ptr<SafeQueue<RequestType>> in_queue,
            std::shared_ptr<ReplyDispatcher<ResponseType>> dispatcher,
            ExtractUUIDFunc uuid_extractor,
            unsigned int timeout_ms = 3000 )
        {
            auto srv = server.lock();
            if( !srv ) return nullptr;

            auto h = std::make_shared<GrpcUnaryAsyncProcessHandler>(
                server, service, cq, request_func, in_queue, dispatcher, uuid_extractor, timeout_ms );
            srv->RegisterHandler( h );
            h->Init();
            return h;
        }

    private:
        std::shared_ptr<SafeQueue<RequestType>>        in_queue_;
        std::shared_ptr<ReplyDispatcher<ResponseType>> dispatcher_;
        ExtractUUIDFunc                                uuid_extractor_;
        unsigned int                                   timeout_ms_;
    };

} // namespace MGEN
