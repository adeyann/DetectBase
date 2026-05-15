#pragma once

#include "SafeThread.h"
#include "SafeQueue.h"
#include "MgenTypes.h"

#include "ServiceBlockProfile.h"
#include "RtspDetectorUnit.h"

#include "IOStreamManager.h"
#include "NetworkManager.h"
#include "EngineLoadBalancer.h"

#include <chrono>
#include <vector>
#include <memory>
#include <set>

namespace MGEN
{
    // V-05: chrono_literals 헤더에서 노출 안 함. default 인자는 std::chrono::milliseconds 명시.

    class RtspDetectorBlock final
    {
    public:
        // default constructor delete
        RtspDetectorBlock() = delete;

        // base constructor
        explicit RtspDetectorBlock(
            const ServiceBlockProfile&          service_block_profile,
            std::shared_ptr<NetworkManager>     network_manager,
            std::shared_ptr<IOStreamManager>    io_stream_manager,
            std::shared_ptr<EngineLoadBalancer> load_balancer
        );

        // destructor
        ~RtspDetectorBlock();

        std::set<MGEN::Type::UnitID> GetServiceUnitIDSet( void );
        bool BuildServiceUnit( const std::chrono::milliseconds each_unit_execute_delay = std::chrono::milliseconds( 10 ) );

        bool IsBuiltByUnit( void ) const noexcept;
        bool Init ( const std::chrono::milliseconds each_unit_execute_delay = std::chrono::milliseconds( 10 ), bool success_need_all_success = false );
        bool Start( const std::chrono::milliseconds each_unit_execute_delay = std::chrono::milliseconds( 10 ), bool success_need_all_success = false );

        // Stop = 일시 정지. 모든 unit 의 Loop thread 정지 (Producer 흐름 차단).
        // 호출 후에도 객체는 살아있고 Start() 로 재시작 가능.
        bool Stop ( const std::chrono::milliseconds each_unit_execute_delay = std::chrono::milliseconds( 10 ), bool success_need_all_success = false );

    private:
        const ServiceBlockProfile profile_;
        std::vector<std::unique_ptr<RtspDetectorUnit>> service_units_;

        std::shared_ptr<NetworkManager>     network_manager_;
        std::shared_ptr<IOStreamManager>    io_stream_manager_;
        std::shared_ptr<EngineLoadBalancer> load_balancer_;
    };

} // namespace MGEN
