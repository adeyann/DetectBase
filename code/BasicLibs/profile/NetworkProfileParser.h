#pragma once

#include <string>
#include <optional>
#include <vector>

namespace MGEN
{
    // GRPC client 가 push 할 대상 endpoint.
    // NetworkSettings.json 의 "GRPC_Peers" 배열에서 파싱.
    struct GrpcPeerEndpoint
    {
        std::string  name = "";   // 식별자 (로그 / 메트릭 label 용)
        std::string  ip   = "";
        unsigned int port = 0;
    };

    struct NetworkProfile
    {
        std::string  local_ip      = "";
        std::string  mvas_ip       = "";
        unsigned int mvas_api_port = 0;
        unsigned int mvas_sio_port = 0;

        // GRPC client 설정. enabled == false 면 grpc_peers 무시.
        // GrpcEventClientBase 인스턴스 생성 자체를 안 해서 영향 0.
        bool                          grpc_client_enabled = false;
        std::vector<GrpcPeerEndpoint> grpc_peers          = {};

        // GRPC server 설정. enabled == false 면 GrpcEventServerBase 인스턴스 생성 안 함.
        // Client 와 독립적으로 ON/OFF 가능 (4 조합 모두 지원).
        bool         grpc_server_enabled      = false;
        std::string  grpc_server_bind_address = "0.0.0.0";
        unsigned int grpc_server_port         = 0;

        void Show( void ) const;
    };

    class NetworkProfileParser
    {
    public:
        using ErrMsg = std::string;

        // network_settings.json 파일에 대한 유효성 검사
        // return:
        //   if all ok, return std::nullopt
        //   if something error, return std::optional<std::string> with error message string
        static std::optional<NetworkProfileParser::ErrMsg> CheckParsable( const std::string& network_settings_json_path );

        // JSON 파일 경로를 파싱하여 NetworkProfile 객체 반환.
        // 실패 시 std::nullopt — 호출자가 검사 후 처리 (throw 사용 안 함).
        static std::optional<NetworkProfile> Parse( const std::string& network_settings_json_path );
    };

} // namespace MGEN
