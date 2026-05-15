#pragma once

#include "MgenProto.grpc.pb.h"
#include "SafeThread.h"

#include <grpcpp/grpcpp.h>
#include <thread>
#include <memory>
#include <atomic>
#include <chrono>
#include <mutex>
#include <functional>

namespace MGEN
{
    class GrpcEventClientBase
    {
    public:
        explicit GrpcEventClientBase( std::shared_ptr<grpc::Channel> channel );
        explicit GrpcEventClientBase( const std::string& target_grpc_server_ip, const int target_grpc_server_port );

        ~GrpcEventClientBase();

        bool Start();
        void Stop();

        // Fire-and-forget
        bool SendEventOnlyJson  ( const EventDataOnlyJson&& event_data );
        bool SendEventWithImages( const EventDataWithRawImages&& event_data );

        // With response
        bool RequestEventOnlyJson  ( const EventDataOnlyJson&& event_data, std::function<void(const std::string&)> on_result );
        bool RequestEventWithImages( const EventDataWithRawImages&& event_data, std::function<void(const std::string&)> on_result );

        // [Phase 4 샘플] Fire-and-forget — counter 변화량 / heartbeat.
        bool SendCounterDelta( const CounterDelta&&  delta );
        bool SendHeartbeat   ( const HeartbeatPing&& ping  );

        // [Phase 4 샘플] Request-Response — counter snapshot 요청. callback 에 CounterSnapshot 전달.
        bool RequestCounterSnapshot( const CounterRequest&& req,
                                     std::function<void(const CounterSnapshot&)> on_result );

    private:
        struct BaseCall
        {
            grpc::ClientContext context;
            grpc::Status status;
            std::string  trace_id;
            std::chrono::steady_clock::time_point start_time;

            virtual ~BaseCall() = default;
        };

        struct SendCall : public BaseCall
        {
            Empty reply;
            std::unique_ptr<grpc::ClientAsyncResponseReader<Empty>> response_reader;
        };

        struct JsonResponseCall : public BaseCall
        {
            EventDataOnlyJson response;
            std::unique_ptr<grpc::ClientAsyncResponseReader<EventDataOnlyJson>> response_reader;
            std::function<void(const std::string&)> callback;
        };

        // [Phase 4 샘플] CounterSnapshot 응답 받는 call.
        struct CounterSnapshotCall : public BaseCall
        {
            CounterSnapshot response;
            std::unique_ptr<grpc::ClientAsyncResponseReader<CounterSnapshot>> response_reader;
            std::function<void(const CounterSnapshot&)> callback;
        };

        std::shared_ptr<grpc::Channel> MakeChannel( const std::string& ip, const int port ) const noexcept;

        void RegistGrpcClientThreadFunctions( void ) noexcept;
        void asyncCompleteRpc();
        void setCommonContext( BaseCall& call, const std::string& uuid ) const noexcept;

    private:
        std::shared_ptr<grpc::Channel>   channel_;
        std::unique_ptr<DETECTBASE_GRPC::Stub> stub_;
        grpc::CompletionQueue cq_;

        SafeThread grpc_client_thread_;
    };
}
