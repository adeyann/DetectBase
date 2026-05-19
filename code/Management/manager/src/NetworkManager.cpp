#include "NetworkManager.h"
#include "MgenLogger.h"
#include "json/json.hpp"

#include "GrpcEventClientBase.h"
#include "GrpcProtoMaker.h"
#include <grpcpp/create_channel.h>
#include <grpcpp/security/credentials.h>

namespace MGEN
{
    NetworkManager::NetworkManager( const NetworkProfile& profile )
        : init_profile_( profile )
    {
        //
    }

    NetworkManager::~NetworkManager()
    {
        this->CloseNetworkAll();
    }

    void NetworkManager::CloseNetworkAll( void )
    {
        if( sio_handler_  ) { sio_handler_->TerminateSocketIO(); }
        if( rtsp_handler_ ) { rtsp_handler_->StopRTSP(); }
        this->CloseGrpcClients();
    }

    // -- GRPC client lifecycle (Phase 1) -------------------------------------

    bool NetworkManager::InitializeGrpcClients( void ) noexcept
    {
        // 이미 init 되었거나 비활성이면 no-op.
        if( !grpc_clients_.empty() ) return true;
        if( !init_profile_.grpc_client_enabled ) {
            MLOG_INFO("   - GRPC Client : disabled (skip init)");
            return true;
        }
        if( init_profile_.grpc_peers.empty() ) {
            MLOG_WARN("   - GRPC Client enabled but no peer configured. Skip.");
            return true;
        }

        // RAID_Analysis 패턴 재사용: reconnect backoff + InsecureChannelCredentials.
        for( const auto& peer : init_profile_.grpc_peers ) {
            try {
                auto client = std::make_shared<GrpcEventClientBase>( peer.ip, static_cast<int>( peer.port ) );
                if( !client->Start() ) {
                    MLOG_WARN("   - GRPC Client[%s] %s:%u Start failed. Skip.",
                        peer.name.c_str(), peer.ip.c_str(), peer.port );
                    continue;
                }
                grpc_clients_.push_back( std::move( client ) );
                MLOG_INFO("   - GRPC Client[%s] connected → %s:%u",
                    peer.name.c_str(), peer.ip.c_str(), peer.port );
            }
            catch( const std::exception& e ) {
                MLOG_ERROR("   - GRPC Client[%s] init exception: %s", peer.name.c_str(), e.what() );
            }
            catch( ... ) {
                // noexcept 시그니처 정합성: std::exception 이 아닌 throw (예: protobuf FatalException) 보호.
                MLOG_ERROR("   - GRPC Client[%s] init unknown exception", peer.name.c_str() );
            }
        }
        return true;
    }

    void NetworkManager::CloseGrpcClients( void ) noexcept
    {
        if( grpc_clients_.empty() ) return;
        for( auto& client : grpc_clients_ ) {
            try { if( client ) client->Stop(); }
            catch( ... ) { /* swallow — shutdown 도중 예외 무시 */ }
        }
        grpc_clients_.clear();
        MLOG_INFO("   - GRPC Clients Closed");
    }

    size_t NetworkManager::BroadcastEventOnlyJsonToGrpcPeers( const std::string& json_payload ) noexcept
    {
        if( grpc_clients_.empty() ) return 0;

        size_t sent = 0;
        for( auto& client : grpc_clients_ ) {
            if( !client ) continue;
            try {
                // GrpcProtoMaker 의 도우미로 string → EventDataOnlyJson 변환.
                client->SendEventOnlyJson( MakeEventOnlyJsonProto( json_payload ) );
                ++sent;
            }
            catch( const std::exception& e ) {
                MLOG_WARN("BroadcastEventOnlyJson exception: %s", e.what() );
            }
            catch( ... ) {
                // noexcept 시그니처 정합성: std::exception 이 아닌 throw (예: protobuf FatalException) 보호.
                MLOG_WARN("BroadcastEventOnlyJson unknown exception" );
            }
        }
        return sent;
    }

    std::optional<ApiSetting> NetworkManager::BuildAPISetting( void )
    {
        ApiSetting api_setting;

        if( this->init_profile_.mvas_ip.empty() ) {
            MLOG_ERROR("%s(), NetworkManager.init_profile has not value 'mvas_ip'", __FUNCTION__ );
            return std::nullopt;
        }

        if( this->init_profile_.local_ip.empty() ) {
            MLOG_ERROR("%s(), NetworkManager.init_profile has not value 'local_ip'", __FUNCTION__ );
            return std::nullopt;
        }

        if( this->init_profile_.mvas_api_port == 0 ) {
            MLOG_ERROR("%s(), NetworkManager.init_profile has not value 'mvas_api_port'", __FUNCTION__ );
            return std::nullopt;
        }

        api_setting.api_service_ip   = this->init_profile_.mvas_ip;
        api_setting.api_service_port = static_cast<int>( this->init_profile_.mvas_api_port );
        api_setting.my_local_ip      = this->init_profile_.local_ip;

        return api_setting;
    }

    std::optional<SioSetting> NetworkManager::BuildSocketIOSetting( const int my_service_id )
    {
        SioSetting sio_setting;

        if( this->init_profile_.mvas_ip.empty() ) {
            MLOG_ERROR("%s(), NetworkManager.init_profile has not value 'mvas_ip'", __FUNCTION__ );
            return std::nullopt;
        }

        if( this->init_profile_.local_ip.empty() ) {
            MLOG_ERROR("%s(), NetworkManager.init_profile has not value 'local_ip'", __FUNCTION__ );
            return std::nullopt;
        }

        if( this->init_profile_.mvas_sio_port == 0 ) {
            MLOG_ERROR("%s(), NetworkManager.init_profile has not value 'mvas_sio_port'", __FUNCTION__ );
            return std::nullopt;
        }

        sio_setting.sio_service_ip   = this->init_profile_.mvas_ip;
        sio_setting.sio_service_port = static_cast<int>( this->init_profile_.mvas_sio_port );
        sio_setting.my_local_ip      = this->init_profile_.local_ip;
        sio_setting.my_server_id     = my_service_id;

        sio_setting.SetDefaultConnectionQuery();

        return sio_setting;
    }

    std::optional<RtspSetting> NetworkManager::BuildRtspSetting( bool camera_xml_is_static_in_service )
    {
        // Phase 1 (GStreamer 기반): happytimesoft 의 XML config (RtspProxyConfigXmlMaker)
        // 는 더 이상 필요 없음 — GStreamer 가 SettingManager 의 카메라 정보를
        // 직접 사용하여 pipeline URL 을 구성. camera_xml_is_static_in_service 는
        // is_rtsp_proxy_static_with_init 으로만 전달 (IOStreamManager 분기용).

        RtspSetting rtsp_setting;
        rtsp_setting.my_local_ip                    = this->init_profile_.local_ip;
        rtsp_setting.is_rtsp_proxy_static_with_init = camera_xml_is_static_in_service;

        return rtsp_setting;
    }

    std::optional<SettingInitData> NetworkManager::BuildSettingManagerInitData( std::string_view entire_service_tag )
    {
        if( !this->api_handler_ )
            return std::nullopt;

        SettingInitData init_data {
            .api_handler = this->api_handler_,
            .service_tag = std::string { entire_service_tag },
            .extend_data = nlohmann::json::object()
        };
        return init_data;
    }

    bool NetworkManager::CreateApiHandler( const ApiSetting& api_setting )
    {
        this->api_handler_ = std::make_shared<ApiHandler>( api_setting );
        if( !this->api_handler_ )
            return false;
        return true;
    }

    bool NetworkManager::InitializeSettingManager( SettingInitData&& init_data )
    {
        auto setting_manager = MGEN::GetSettingManager();
        if( !setting_manager )
            return false;
        return setting_manager->Initialize( std::move( init_data ) );
    }

    bool NetworkManager::CreateSioHandler( const SioSetting& sio_setting )
    {
        this->sio_handler_ = std::make_shared<SioHandler>( sio_setting );
        if( !this->sio_handler_ )
            return false;
        if( this->sio_handler_->Initialize() == false )
            return false;
        return true;
    }

    bool NetworkManager::CreateRtspHandler( const RtspSetting& rtsp_setting )
    {
        this->rtsp_handler_ = std::make_shared<RtspHandler>( rtsp_setting );
        if( !this->rtsp_handler_ )
            return false;
        if( this->rtsp_handler_->Initialize() == false )
            return false;
        return true;
    }

    /*
        1. Connect MVAS API service ( mvas_api )
        2. Load DB use from 'mvas_api' & Set setting data to SettingManager ( include ServiceID )
        3. Connect MVAS SIO service ( mvas_broker )
    */
    bool NetworkManager::ConnMVAS( std::string_view entire_service_tag )
    {
        auto opt_api_setting = this->BuildAPISetting();
        if( opt_api_setting.has_value() == false ) {
            MLOG_ERROR("%s()::BuildAPISetting() failed.", __func__ );
            return false;
        }

        if( this->CreateApiHandler( *opt_api_setting ) == false ) {
            MLOG_ERROR("%s()::CreateApiHandler() failed.", __func__ );
            return false;
        }
        MLOG_INFO("   - Conncect to MVAS API service ( mvas_api ) - %s:%d",
            init_profile_.mvas_ip.c_str(), init_profile_.mvas_api_port );

        auto opt_sm_init_data = this->BuildSettingManagerInitData( entire_service_tag );
        if( opt_sm_init_data.has_value() == false ) {
            MLOG_ERROR("%s()::BuildSettingManagerInitData() failed.", __func__ );
            return false;
        }

        if( this->InitializeSettingManager( std::move( *opt_sm_init_data ) ) == false ) {
            MLOG_ERROR("%s()::InitializeSettingManager() failed.", __func__ );
            return false;
        }

        auto setting_manager = MGEN::GetSettingManager();
        auto opt_sio_setting = this->BuildSocketIOSetting( setting_manager->GetServerServiceID() );
        if( opt_sio_setting.has_value() == false ){
            MLOG_ERROR("%s()::BuildSocketIOSetting() failed.", __func__ );
            return false;
        }

        if( this->CreateSioHandler( *opt_sio_setting ) == false ) {
            MLOG_ERROR("%s()::CreateSioHandler() failed.", __func__ );
            return false;
        }

        // GRPC client 활성 시 peer 연결 시도 (실패는 비치명적 — Start 실패한 peer 만 skip).
        this->InitializeGrpcClients();

        this->entire_service_tag_name_ = std::string { entire_service_tag };
        return true;
    }

    bool NetworkManager::InitializeRTSP( bool camera_xml_is_static_in_service )
    {
        auto opt_rtsp_setting = this->BuildRtspSetting( camera_xml_is_static_in_service );
        if( opt_rtsp_setting.has_value() == false ) {
            MLOG_ERROR("%s()::BuildRtspSetting() failed.", __func__ );
            return false;
        }

        if( this->CreateRtspHandler( *opt_rtsp_setting ) == false ) {
            MLOG_ERROR("%s()::CreateRtspHandler() failed.", __func__ );
            return false;
        }

        MLOG_INFO("   - Initialize RTSP successfully." );
        return true;
    }

    bool NetworkManager::InitializeRTSPWithStaticCameraList( void )
    {
        const bool camera_xml_is_static_in_service = true;
        return InitializeRTSP( camera_xml_is_static_in_service );
    }

} // namespace MGEN
