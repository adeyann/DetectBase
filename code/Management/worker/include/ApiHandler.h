#pragma once

// BasicLibs
#include "InterProtocolTypes.h"

// REST
#include "rest_connection.h"
#include "rest_client.h"

// JSON
#include "json/json_fwd.hpp"

#include <string>
#include <string_view>
#include <memory>
#include <mutex>
#include <tuple>
#include <set>
#include <functional>
#include <optional>
#include <vector>   // _make_api_url_param에서 사용
#include <chrono>   // timeoutDuration, steady_clock 등에서 사용

namespace MGEN
{
    // Connection settings : default
    constexpr int DEFAULT_REST_API_GET_TIMEOUT_SECS = 3;

    using API_RSP_CODE = int;

    namespace API
    {
        enum class URL
        {
            NOT_SET,
            // REST API endpoints (general)
            API_GET_SERVER_BY_IP,
            API_GET_SERVER_BY_ID,
            API_GET_CLUSTER,
            API_GET_EXCEPTION_CAMERA,
            API_GET_SCHEDULE,
            API_GET_CAMERA,
        };

        std::string _make_api_url_basic( const MGEN::API::URL       enum_tag ) noexcept;

        std::string _make_api_url_param( const std::vector<std::string>& values ) noexcept;
        std::string _make_api_url_param( const std::vector<int>& values ) noexcept;
        std::string _make_api_url_param( const std::set<int>& values ) noexcept;
        std::string _make_api_url_param( const std::string& single_value ) noexcept;
        std::string _make_api_url_param( std::string_view single_value ) noexcept;
        std::string _make_api_url_param( const int& single_value ) noexcept;

        template <typename T>
        std::string MakeURL( const MGEN::API::URL enum_tag, const T& params )
        {
            std::string prefix = MGEN::API::_make_api_url_basic( enum_tag );
            std::string suffix = MGEN::API::_make_api_url_param( params );

            return prefix + suffix;
        }
    }


    struct ApiSetting
    {
        std::string api_service_ip;   // target main server(mvas_api) ip
        int         api_service_port; // target main server(mvas_api) REST API port
        std::string my_local_ip;      // current service local ip
    };

    using UrlBuilderFunc = std::function<std::string(const nlohmann::json&)>;

    class ApiHandler
    {
    public:
        // default constructor delete
        ApiHandler() = delete;

        // base constructor
        explicit ApiHandler( const ApiSetting& setting ) noexcept;

        // shared_ptr 로 관리되므로 복사/이동 모두 금지
        ApiHandler( const ApiHandler& ) = delete;
        ApiHandler& operator=( const ApiHandler& ) = delete;
        ApiHandler( ApiHandler&& ) = delete;
        ApiHandler& operator=( ApiHandler&& ) = delete;

        // destructor
        ~ApiHandler() = default;

        // Check alive
        bool IsAlive( void );

        // set timeout
        void SetTimeout( const int to );

        // GET (historical name — 현재는 throw 하지 않음. timeout / fail 시 code <= 0 반환).
        // 이름 retain 사유: 호출자 다수 (SettingManager / SioEventBinder 등). rename 시 큰 변경.
        // 호출자의 try/catch 는 nlohmann::json 접근 throw 보호 차원으로 유지됨.
        std::tuple<API_RSP_CODE, nlohmann::json> GET_or_throw_if_timeout( std::string_view url, int timeoutSeconds = DEFAULT_REST_API_GET_TIMEOUT_SECS );
        std::tuple<API_RSP_CODE, nlohmann::json> GET_or_throw_if_timeout( std::string_view url, std::chrono::seconds timeoutDuration );
        std::tuple<API_RSP_CODE, nlohmann::json> GET_or_throw_if_timeout( std::string_view url, std::chrono::time_point<std::chrono::steady_clock> timeoutTime );

        std::string GetMyLocalIp( void );

        // Response status checker
        static bool IsGoodResponse( const API_RSP_CODE code ) noexcept;
        static bool IsKnownResponseCode( const API_RSP_CODE code ) noexcept;

        // Build callback function use api_handler instance
        InterProtocolFunc BuildCallback_GET( InterProtocolInputChecker checker, UrlBuilderFunc url_builder );
        // internal BuildCallback_GET
        std::optional<nlohmann::json> _BC_GET_Internal( const nlohmann::json& req_data, InterProtocolInputChecker checker, UrlBuilderFunc url_builder );

    protected:
        static std::once_flag init_flag;

        ApiSetting setting;
        std::unique_ptr<RestClient::Connection> conn;
        mutable std::mutex mtx;
    };

}
