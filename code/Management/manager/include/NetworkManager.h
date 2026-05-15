#pragma once

#include "RtspHandler.h"
#include "ApiHandler.h"
#include "SioHandler.h"

#include "NetworkProfileParser.h" // NetworkProfile
#include "ServiceProfile.h"
#include "SettingManager.h"

#include <memory> // unique_ptr, shared_ptr
#include <optional>
#include <string_view>
#include <vector>

namespace MGEN
{
    // forward declaration. .cpp 에서만 grpc 헤더 include —
    // NetworkManager 의 외부 사용자에게 grpc 의존성 노출 안 함.
    class GrpcEventClientBase;

    class NetworkManager
    {
    public:
        // non param constructor deleted
        NetworkManager( void ) = delete;

        // base constructor
        explicit NetworkManager( const NetworkProfile& profile );

        // destructor
        ~NetworkManager();

        // Prevent copying and assignment
        NetworkManager(const NetworkManager&) = delete;
        NetworkManager& operator=(const NetworkManager&) = delete;
        NetworkManager(NetworkManager&&) = delete;
        NetworkManager& operator=(NetworkManager&&) = delete;

        // Mgensolution 메인 서버( MVAS ) 연결 및 연결 핸들러 생성
        bool ConnMVAS( std::string_view entire_service_tag );

        // 서비스 초기화 시 MVAS에서 불러온 카메라 목록값을 기준으로 RTSP 초기화 하는 방법
        bool InitializeRTSPWithStaticCameraList( void );

        // CloseNetworkAll = 영구 정리. SocketIO sync_close + RTSP stop + thread join.
        // idempotent: 두 번 호출되어도 안전 (이미 close 된 핸들러는 no-op).
        // destructor 에서 자동 호출됨 (RAII).
        void CloseNetworkAll( void );

        // get handlers
        std::shared_ptr<ApiHandler>  GetApiHandler ( void ) { return this->api_handler_;  }
        std::shared_ptr<SioHandler>  GetSioHandler ( void ) { return this->sio_handler_;  }
        std::shared_ptr<RtspHandler> GetRtspHandler( void ) { return this->rtsp_handler_; }

        // -- GRPC client (Phase 1) --------------------------------------
        // GRPC client 활성화 여부. NetworkSettings.json 의 GRPC_Client_Enabled 기반.
        bool   IsGrpcClientEnabled( void ) const { return !grpc_clients_.empty(); }
        size_t GetGrpcPeerCount  ( void ) const { return grpc_clients_.size(); }

        // 활성 peer 모두에게 fire-and-forget JSON 이벤트 broadcast.
        // 비활성 (grpc_clients_ 비어있음) 시 즉시 반환 (no-op).
        // payload: SocketIO emit 과 동일한 JSON 문자열을 그대로 GRPC EventDataOnlyJson::json_data 에 담음.
        // 반환값: 송신 시도된 peer 수 (실제 도착 보장 안 함 — fire-and-forget).
        size_t BroadcastEventOnlyJsonToGrpcPeers( const std::string& json_payload ) noexcept;

        // GRPC client lifecycle. ConnMVAS 안에서 자동 호출되지만 명시 호출도 가능.
        // 비활성 시 no-op.
        bool InitializeGrpcClients( void ) noexcept;
        void CloseGrpcClients     ( void ) noexcept;

    private:
        bool CreateApiHandler ( const ApiSetting&  api_setting );
        bool CreateSioHandler ( const SioSetting&  sio_setting  );
        bool CreateRtspHandler( const RtspSetting& rtsp_setting );
        bool InitializeSettingManager( SettingInitData&& init_data );

        bool InitializeRTSP( bool camera_xml_is_static_in_service );

        std::optional<ApiSetting>      BuildAPISetting( void );
        std::optional<SioSetting>      BuildSocketIOSetting( const int my_service_id );
        std::optional<RtspSetting>     BuildRtspSetting( bool camera_xml_is_static_in_service = true );
        std::optional<SettingInitData> BuildSettingManagerInitData( std::string_view entire_service_tag );

    private:
        const NetworkProfile init_profile_;
        std::string entire_service_tag_name_;

        std::shared_ptr<ApiHandler>  api_handler_  = nullptr;
        std::shared_ptr<SioHandler>  sio_handler_  = nullptr;
        std::shared_ptr<RtspHandler> rtsp_handler_ = nullptr;

        // peer 별 GrpcEventClientBase 인스턴스. 비활성 시 빈 벡터.
        std::vector<std::shared_ptr<GrpcEventClientBase>> grpc_clients_;
    };

}