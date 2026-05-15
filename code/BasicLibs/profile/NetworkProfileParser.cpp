#include "NetworkProfileParser.h"
#include "MgenLogger.h"
#include "file_utils.h"
#include "ip_utils.h"
#include "json/json.hpp"

#include <fstream>

/*
{
    "MVAS_IP": "http://192.168.1.104",
    "MVAS_IP_INFO": "MVAS의 ip 주소. http인지 https 인지 프로토콜 포함",

    "SocketIO_Port": 3335,
    "SocketIO_Port_INFO": "MVAS 서비스 중 SocketIO 의 포트 번호",

    "REST_API_Port": 8005,
    "REST_API_Port_INFO": "MVAS 서비스 중 REST API 의 포트 번호",

    "LocalIP": "192.168.1.104",
    "LocalIP_INFO_1": "현재 서버의 로컬 IP. 해당 IP를 기준으로 내 서비스 ID 등 MVAS 쿼리 요청함.",
    "LocalIP_INFO_2": "만약 위의 IP값을 할당하지 않을 시 자동 탐색은 시행하지만, 다른 IP 할당 가능성 있음."
}
*/

namespace MGEN
{
    using json = nlohmann::json;

    std::optional<NetworkProfileParser::ErrMsg> NetworkProfileParser::CheckParsable( const std::string& network_settings_json_path )
    {
        const std::string& json_path = network_settings_json_path;

        if( IsValidFile( json_path ) == false ){
            return "CheckParsable(), NetworkSettings 파일이 존재하지 않거나 잘못된 경로입니다: " + json_path;
        }

        std::ifstream settings_ifs { json_path };
        if( !settings_ifs ){
            return "CheckParsable(), NetworkSettings 파일을 열 수 없습니다: " + json_path;
        }

        json settings_json;
        try {
            settings_ifs >> settings_json;
        }
        catch ( const std::exception& e ){
            return std::string("CheckParsable(), JSON 파싱 오류: ") + e.what();
        }

        const std::string v_1 = settings_json.value("MVAS_IP", "");
        if( v_1.empty() ){
            return "CheckParsable(), 'MVAS_IP' 키가 누락되었거나 값이 비어 있습니다.";
        }

        const unsigned int v_2 = settings_json.value("SocketIO_Port", 0);
        if( v_2 == 0 ) {
            return "CheckParsable(), 'SocketIO_Port' 키가 누락되었거나 값이 비어 있습니다.";
        }

        const unsigned int v_3 = settings_json.value("REST_API_Port", 0);
        if( v_3 == 0 ) {
            return "CheckParsable(), 'REST_API_Port' 키가 누락되었거나 값이 비어 있습니다.";
        }

        return std::nullopt;
    }

    std::optional<NetworkProfile> NetworkProfileParser::Parse( const std::string& network_settings_json_path )
    {
        const auto& json_path = network_settings_json_path;
        if( NetworkProfileParser::CheckParsable( json_path ) != std::nullopt ) {
            MLOG_ERROR( "NetworkProfileParser::Parse - Target json file not parsable: %s", json_path.c_str() );
            return std::nullopt;
        }

        NetworkProfile profile {};

        std::ifstream settings_ifs { json_path };
        if( !settings_ifs ){
            MLOG_ERROR( "NetworkProfileParser::Parse - Failed to open json: %s", json_path.c_str() );
            return std::nullopt;
        }

        json settings_json;
        settings_ifs >> settings_json;

        // Local IP first..
        profile.local_ip = settings_json.value("LocalIP", "");
        if( profile.local_ip.empty() ) {
            MLOG_WARN("network_settings.json has not key 'LocalIP', try find IP automatically...");
            profile.local_ip = getPhysicalIPAddress();
            MLOG_INFO("Find IP : %s", profile.local_ip.c_str() );
        }

        // MVAS setting
        profile.mvas_ip       = settings_json.value("MVAS_IP", "");
        profile.mvas_sio_port = settings_json.value("SocketIO_Port", 0);
        profile.mvas_api_port = settings_json.value("REST_API_Port", 0);

        // GRPC client setting (선택 항목 — 누락 시 비활성).
        profile.grpc_client_enabled = settings_json.value( "GRPC_Client_Enabled", false );
        profile.grpc_peers.clear();
        if( profile.grpc_client_enabled ) {
            const auto peers_it = settings_json.find( "GRPC_Peers" );
            if( peers_it != settings_json.end() && peers_it->is_array() ) {
                for( const auto& peer_js : *peers_it ) {
                    if( !peer_js.is_object() ) continue;
                    GrpcPeerEndpoint peer;
                    peer.name = peer_js.value( "name", "" );
                    peer.ip   = peer_js.value( "ip",   "" );
                    peer.port = peer_js.value( "port", 0u );
                    if( peer.ip.empty() || peer.port == 0 ) {
                        MLOG_WARN( "GRPC_Peers entry skipped (ip/port invalid): name='%s'", peer.name.c_str() );
                        continue;
                    }
                    if( peer.name.empty() ) {
                        peer.name = peer.ip + ":" + std::to_string( peer.port );
                    }
                    profile.grpc_peers.push_back( std::move( peer ) );
                }
            }
            if( profile.grpc_peers.empty() ) {
                MLOG_WARN( "GRPC_Client_Enabled=true but no valid peer found. Disabling." );
                profile.grpc_client_enabled = false;
            }
        }

        // GRPC server setting (선택 항목 — 누락 시 비활성).
        profile.grpc_server_enabled = settings_json.value( "GRPC_Server_Enabled", false );
        if( profile.grpc_server_enabled ) {
            profile.grpc_server_bind_address = settings_json.value( "GRPC_Server_BindAddress", std::string{ "0.0.0.0" } );
            profile.grpc_server_port         = settings_json.value( "GRPC_Server_Port", 0u );
            if( profile.grpc_server_port == 0 ) {
                MLOG_WARN( "GRPC_Server_Enabled=true but no valid port. Disabling." );
                profile.grpc_server_enabled = false;
            }
        }

        return profile;
    }

    void NetworkProfile::Show( void ) const
    {
        const std::string my_local_ip       = this->local_ip.empty()     ? "Not Set" : this->local_ip;
        const std::string mvas_srv_ip       = this->mvas_ip.empty()      ? "Not Set" : this->mvas_ip;
        const std::string mvas_api_port_str = (this->mvas_api_port == 0) ? "Not Set" : std::to_string( this->mvas_api_port );
        const std::string mvas_sio_port_str = (this->mvas_sio_port == 0) ? "Not Set" : std::to_string( this->mvas_sio_port );

        MLOG_INFO("   - Local IP           : %s", my_local_ip.c_str() );
        MLOG_INFO("   - MVAS IP            : %s", mvas_srv_ip.c_str() );
        MLOG_INFO("   - MVAS API Port      : %s", mvas_api_port_str.c_str() );
        MLOG_INFO("   - MVAS SocketIO Port : %s", mvas_sio_port_str.c_str() );

        if( this->grpc_client_enabled ) {
            MLOG_INFO("   - GRPC Client        : ENABLED (peers: %zu)", this->grpc_peers.size() );
            for( const auto& peer : this->grpc_peers ) {
                MLOG_INFO("       - %s @ %s:%u", peer.name.c_str(), peer.ip.c_str(), peer.port );
            }
        } else {
            MLOG_INFO("   - GRPC Client        : disabled" );
        }

        if( this->grpc_server_enabled ) {
            MLOG_INFO("   - GRPC Server        : ENABLED (bind: %s:%u)",
                this->grpc_server_bind_address.c_str(), this->grpc_server_port );
        } else {
            MLOG_INFO("   - GRPC Server        : disabled" );
        }
    }

} // namespace MGEN
