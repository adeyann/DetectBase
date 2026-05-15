#include "IOStreamManager.h"
#include "json/json.hpp"
#include <iostream>
#include <algorithm>
#include <iterator>

namespace MGEN
{
    std::shared_ptr<GrpcEventJsonDispatcher> IOStreamManager::GetDispatcher( const std::string& key )
    {
        std::lock_guard<std::mutex> lock(map_mutex_); // 뮤텍스 잠금
        // lookup 함수를 사용하여 맵에서 안전하게 검색
        return lookup( grpc_server_respond_dispatcher_map_, key );
    }

    void IOStreamManager::ClearAll()
    {
        std::lock_guard<std::mutex> lock(map_mutex_); // 뮤텍스 잠금
        for( auto& [ _, q ] : sio_event_internal_q_map_ ) {
            q->terminate();
        }
        sio_event_internal_q_map_.clear();

        for( auto& [ _, q ] : grpc_client_respond_internal_q_map_ ) {
            q->terminate();
        }
        grpc_client_respond_internal_q_map_.clear();

        for( auto& [ _, q ] : grpc_server_request_json_internal_q_map_ ) {
            q->terminate();
        }
        grpc_server_request_json_internal_q_map_.clear();

        for( auto& [ _, q ] : grpc_server_request_image_internal_q_map_ ) {
            q->terminate();
        }
        grpc_server_request_image_internal_q_map_.clear();

        for( auto& [ _, d ] : grpc_server_respond_dispatcher_map_ ) {
            d->StopAutoCleanup();
        }
        grpc_server_respond_dispatcher_map_.clear();

        for( auto& [ _, q ] : rtsp_proxy_frame_internal_q_map_ ) {
            q->terminate();
        }
        rtsp_proxy_frame_internal_q_map_.clear();
    }

    std::set<MGEN::Type::UnitID> IOStreamManager::RegisterRtspProxies( std::set<MGEN::Type::UnitID> unit_set )
    {
        std::set<MGEN::Type::UnitID> success_set;
        for( const auto unit_id : unit_set ) {
            if( this->RegisterSafeQueue( QueueType::RTSP_AVFRAME, unit_id ) == true )
                success_set.insert( unit_id );
        }
        return success_set;
    }

    std::set<MGEN::Type::UnitID> IOStreamManager::UnregisterRtspProxies( std::set<MGEN::Type::UnitID> unit_set )
    {
        std::set<MGEN::Type::UnitID> success_set;
        for( const auto unit_id : unit_set ) {
            if( this->UnregisterSafeQueue( QueueType::RTSP_AVFRAME, unit_id ) == true )
                success_set.insert( unit_id );
        }
        return success_set;
    }

    bool IOStreamManager::ReadyImpl_SIO( const ServiceProfile& service_profile, const std::shared_ptr<NetworkManager>& network_manager )
    {
        if( !network_manager )
            return false;

        auto api_handler = network_manager->GetApiHandler();
        auto sio_handler = network_manager->GetSioHandler();

        if( !api_handler || !sio_handler )
            return false;

        this->is_ready_socket_io_pipeline_ = true;
        return true;
    }

    bool IOStreamManager::ReadyImpl_RTSP( const ServiceProfile& /*service_profile*/, const std::shared_ptr<NetworkManager>& network_manager )
    {
        if( !network_manager )
            return false;

        auto rtsp_handler = network_manager->GetRtspHandler();
        if( !rtsp_handler )
            return false;

        if( rtsp_handler->IsProxySettingStaticUseInitData() == false )
            return true;

        auto setting_manager = MGEN::GetSettingManager();
        if( !setting_manager )
            return false;

        auto register_proxy_ids_rtsp_side = rtsp_handler->GetProxyIDs();
        auto register_proxy_ids_mvas_side = setting_manager->GetCameraIDSet();

        std::set<MGEN::Type::UnitID> register_proxy_ids;

        std::set_union(
            register_proxy_ids_rtsp_side.begin(), register_proxy_ids_rtsp_side.end(),
            register_proxy_ids_mvas_side.begin(), register_proxy_ids_mvas_side.end(),
            std::inserter( register_proxy_ids, register_proxy_ids.begin() )
        );

        auto registed_proxy_ids = this->RegisterRtspProxies( register_proxy_ids );
        bool success = ( registed_proxy_ids == register_proxy_ids );

        if( success ) {
            this->is_ready_rtsp_pipeline_ = true;
            return true;
        }
        return false;
    }


    bool IOStreamManager::Ready( const ServiceProfile& service_profile, const std::shared_ptr<NetworkManager>& network_manager )
    {
        // Check NetworkManager
        if( !network_manager ) {
            MLOG_ERROR("IOStreamManager::Ready() Failed, network_manager == nullptr");
            return false;
        }

        // SocketIO
        if( network_manager->GetSioHandler() != nullptr && this->IsReadyPipelineSocketIO() == false ) {
            if( this->ReadyImpl_SIO( service_profile, network_manager ) == false ) {
                MLOG_ERROR("IOStreamManager::Ready::ReadyImpl_SIO() Failed");
                return false;
            }
        }

        // RTSP
        if( network_manager->GetRtspHandler() != nullptr && this->IsReadyPipelineRTSP() == false ) {
            if( this->ReadyImpl_RTSP( service_profile, network_manager ) == false ) {
                MLOG_ERROR("IOStreamManager::Ready::ReadyImpl_RTSP() Failed");
                return false;
            }
        }

        // GRPC

        return true;
    }

} // namespace MGEN

