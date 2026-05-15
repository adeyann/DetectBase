#pragma once

// MGEN::BasicLibs
#include "ServiceProfileBuilder.h" // ServiceProfile
#include "NetworkProfileParser.h"  // NetworkProfile, NetworkProfileParser
#include "EngineProfile.h"         // EngineProfile
#include "EngineProfileParser.h"   // EngineProfileParser

// MGEN::Management
#include "NetworkManager.h"        // NetworkProfile, NetworkManager
#include "EngineLoadBalancer.h"    // EngineLoadBalancer
#include "IOStreamManager.h"       // IOStreamManager
#include "EngineHandlerBase.h"     // EngineHandlerBase

// MGEN::Protocol — Phase 3 GRPC server
#include "GrpcEventServerBase.h"

// MGEN::Main
#include "InitMain.h"

// Service
#include "RtspDetectorBlock.h"

// STL::C++
#include <condition_variable>
#include <mutex>
#include <string>
#include <vector>
#include <memory>
#include <atomic>

namespace MGEN
{
    class Service_DETECTOR final
    {
    public:
        //
        explicit Service_DETECTOR( void );

        // unique_ptr<GrpcEventServerBase> 가 forward decl 이라 dtor 정의는 .cpp 에서
        // (그 시점에 GrpcEventServerBase 가 complete type).
        ~Service_DETECTOR();

        //
        bool Initialize( void );
        bool Run( void );
        bool WaitUntilQuitSignal( std::atomic<bool>& signal_flag );
        bool Quit( void );

    // helper
    private:
        bool SocketIOEventBind( void );

    private:
        // Profile
        ServiceProfile service_profile_;
        std::string    service_version_;
        NetworkProfile network_profile_;
        std::vector<EngineProfile> engine_profiles_;

        // Manager
        std::shared_ptr<NetworkManager>     network_manager_   = nullptr;
        std::shared_ptr<IOStreamManager>    io_stream_manager_ = nullptr;
        std::shared_ptr<EngineLoadBalancer> load_balancer_     = nullptr;

        // Engine
        std::vector<std::unique_ptr<EngineHandlerBase>> engines_;

        // Service Implement
        std::unique_ptr<RtspDetectorBlock> detector_block_  = nullptr;

        // GRPC server (Phase 3) — 활성 시 인스턴스. 비활성 시 nullptr.
        // W-13: shared_ptr 로 변경. enable_shared_from_this 사용 위해 필수.
        std::shared_ptr<GrpcEventServerBase> grpc_server_ = nullptr;

        // wait sig
        mutable std::mutex mtx_;
        std::condition_variable cond_;
        std::atomic<bool> is_quit_ { false };
    };
} // namespace MGEN