#pragma once

#include "MgenProto.grpc.pb.h"
#include "SafeQueue.h"
#include "SafeThread.h"
#include "ReplyDispatcher.h"

#include <grpcpp/grpcpp.h>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace MGEN
{
    // W-13 fix: enable_shared_from_this 상속.
    //   handler 가 weak_ptr<GrpcEventServerBase> 보유 → server destroy 후 dangling 차단.
    //   shared_ptr 으로만 생성해야 (DETECTOR 가 make_shared<GrpcEventServerBase>).
    class GrpcEventServerBase : public std::enable_shared_from_this<GrpcEventServerBase>
    {
    public:
        explicit GrpcEventServerBase(const std::string& server_ip, const int server_port );
        ~GrpcEventServerBase();

        void Run();
        void Stop();

        // Setter for fire-and-forget queues
        void SetSendEventOnlyJsonQueue  ( std::shared_ptr<SafeQueue<EventDataOnlyJson>>      in );
        void SetSendEventWithImagesQueue( std::shared_ptr<SafeQueue<EventDataWithRawImages>> in );

        void SetSendEventOnlyJsonPostProcesser  ( std::function<void(const EventDataOnlyJson&, Empty&)>      post_processor );
        void SetSendEventWithImagesPostProcesser( std::function<void(const EventDataWithRawImages&, Empty&)> post_processor );

        // Setter for request/response pipelines
        void SetRequestEventOnlyJsonQueues  ( std::shared_ptr<SafeQueue<EventDataOnlyJson>>      in, std::shared_ptr<ReplyDispatcher<EventDataOnlyJson>> out );
        void SetRequestEventWithImagesQueues( std::shared_ptr<SafeQueue<EventDataWithRawImages>> in, std::shared_ptr<ReplyDispatcher<EventDataOnlyJson>> out );

        // [Phase 4 샘플] post-processor (외부에서 set 안 하면 default = 로그만 출력).
        void SetSendCounterDeltaPostProcesser    ( std::function<void(const CounterDelta&,    Empty&)>           post );
        void SetSendHeartbeatPostProcesser       ( std::function<void(const HeartbeatPing&,   Empty&)>           post );
        void SetRequestCounterSnapshotHandler    ( std::function<void(const CounterRequest&,  CounterSnapshot&)> post );

    public:
        // Phase 2 (분석 6 fix): 모든 handler 가 shared_ptr 로 관리됨.
        // server 의 alive_handlers_ 가 lifetime 보유. detached thread 는 shared_from_this() capture →
        // thread 살아있는 한 handler 객체 살아있음 → UAF 차단.
        class CallDataBase : public std::enable_shared_from_this<CallDataBase> {
        public:
            virtual void Proceed(bool ok) = 0;
            virtual ~CallDataBase() = default;
        };

        // -- handler registry (Phase 2) ----------------------------------
        // handler 가 자기 등록. server 가 shared_ptr 보유 → ref 유지.
        void RegisterHandler  ( std::shared_ptr<CallDataBase> handler );

        // handler 가 자기 raw 로 unregister 요청 → map erase → ref count -1.
        // 다른 곳 (detached thread 등) 에서 capture 한 shared_ptr 이 없으면 즉시 delete.
        void UnregisterHandler( CallDataBase* raw_handler );

        // shutdown 시 일괄 정리. detached thread 가 capture 한 shared_ptr 만 남음 (thread 종료 시 자동 정리).
        void ClearAllHandlers ( void );

    private:
        std::string MakeUrl( const std::string& ip, const int port ) const noexcept;

        void RegistGrpcClientThreadFunctions( void ) noexcept;
        void HandleRpcsRunner();
        void HandleRpcsCloser();
        void DrainCompletionQueue();

        std::string server_address_;
        std::unique_ptr<grpc::Server> server_;
        std::unique_ptr<grpc::ServerCompletionQueue> cq_;
        DETECTBASE_GRPC::AsyncService service_;
        MGEN::SafeThread grpc_server_thread_;

        // Phase 2: handler lifetime 보유. key = raw pointer (cq tag 와 동일), value = shared_ptr.
        std::unordered_map<void*, std::shared_ptr<CallDataBase>> alive_handlers_;
        mutable std::mutex                                       alive_mtx_;

        // Queues for fire-and-forget
        std::shared_ptr<SafeQueue<EventDataOnlyJson>>              queue_send_event_json_;
        std::shared_ptr<SafeQueue<EventDataWithRawImages>>         queue_send_event_images_;
        std::function<void(const EventDataOnlyJson&,      Empty&)> send_event_json_post_process_;
        std::function<void(const EventDataWithRawImages&, Empty&)> send_event_images_post_process_;

        // Queues for request-reply
        std::shared_ptr<SafeQueue<EventDataOnlyJson>>       queue_request_event_json_in_;
        std::shared_ptr<SafeQueue<EventDataWithRawImages>>  queue_request_event_images_in_;
        std::shared_ptr<ReplyDispatcher<EventDataOnlyJson>> dispatcher_event_json_;
        std::shared_ptr<ReplyDispatcher<EventDataOnlyJson>> dispatcher_event_images_;

        // [Phase 4 샘플] handler 들. set 안 되면 default = 로그만.
        std::function<void(const CounterDelta&,    Empty&)>           send_counter_delta_post_process_;
        std::function<void(const HeartbeatPing&,   Empty&)>           send_heartbeat_post_process_;
        std::function<void(const CounterRequest&,  CounterSnapshot&)> request_counter_snapshot_handler_;
    };

} // namespace MGEN
