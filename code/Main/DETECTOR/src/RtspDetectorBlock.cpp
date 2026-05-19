#include "RtspDetectorBlock.h"

#include <atomic>
#include <thread>

namespace MGEN
{
    RtspDetectorBlock::RtspDetectorBlock
    (
        const ServiceBlockProfile&          service_block_profile,
        std::shared_ptr<NetworkManager>     network_manager,
        std::shared_ptr<IOStreamManager>    io_stream_manager,
        std::shared_ptr<EngineLoadBalancer> load_balancer
    )
        : profile_          ( service_block_profile )
        , network_manager_  ( std::move( network_manager ) )
        , io_stream_manager_( std::move( io_stream_manager ) )
        , load_balancer_    ( std::move( load_balancer ) )
    {
        //
    }

    RtspDetectorBlock::~RtspDetectorBlock()
    {
        this->Stop();
    }

    std::set<MGEN::Type::UnitID> RtspDetectorBlock::GetServiceUnitIDSet( void )
    {
        return MGEN::GetSettingManager()->GetCameraIDSet();
    }

    bool RtspDetectorBlock::BuildServiceUnit( const std::chrono::milliseconds each_unit_execute_delay )
    {
        auto cam_set = this->GetServiceUnitIDSet();

        for( const auto& cam_id : cam_set ) {
            service_units_.push_back( std::make_unique<RtspDetectorUnit>(
                cam_id, profile_, network_manager_, io_stream_manager_, load_balancer_
            ) );
            std::this_thread::sleep_for( each_unit_execute_delay );
        }

        if( service_units_.size() == cam_set.size() )
            return true;
        return false;
    }

    bool RtspDetectorBlock::IsBuiltByUnit( void ) const noexcept
    {
        return profile_.GetServiceBlockBuildType() == ServiceBlockBuildType::ByUnit;
    }

    bool RtspDetectorBlock::Init( const std::chrono::milliseconds each_unit_execute_delay, bool success_need_all_success )
    {
        const size_t total_unit_size = service_units_.size();
        if( total_unit_size == 0 )
            return false;

        std::atomic<size_t> success_count { 0 };
        for( auto& unit_ptr : service_units_ ) {
            if( unit_ptr->Init() == true ) {
                success_count++;
            }
            std::this_thread::sleep_for( each_unit_execute_delay );
        }

        if( success_need_all_success )
            return success_count.load() == total_unit_size;
        else
            return success_count.load() != 0;
    }

    bool RtspDetectorBlock::Start( const std::chrono::milliseconds each_unit_execute_delay, bool success_need_all_success )
    {
        const size_t total_unit_size = service_units_.size();
        if( total_unit_size == 0 )
            return false;

        std::atomic<size_t> success_count { 0 };
        for( auto& unit_ptr : service_units_ ) {
            if( unit_ptr->Start() == true ) {
                success_count++;
            }
            std::this_thread::sleep_for( each_unit_execute_delay );
        }

        if( success_need_all_success )
            return success_count.load() == total_unit_size;
        else
            return success_count.load() != 0;
    }

    bool RtspDetectorBlock::Stop( const std::chrono::milliseconds each_unit_execute_delay, bool success_need_all_success )
    {
        const size_t total_unit_size = service_units_.size();
        if( total_unit_size == 0 )
            return false;

        std::atomic<size_t> success_count { 0 };
        for( auto& unit_ptr : service_units_ ) {
            if( unit_ptr->Stop() == true ) {
                success_count++;
            }
            std::this_thread::sleep_for( each_unit_execute_delay );
        }

        if( success_need_all_success )
            return success_count.load() == total_unit_size;
        else
            return success_count.load() != 0;
    }

}
