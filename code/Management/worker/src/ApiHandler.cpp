#include "ApiHandler.h"
#include "rest_impl.h"
#include "json/json.hpp"

namespace MGEN
{
    std::once_flag ApiHandler::init_flag;

    using std::chrono::time_point;
    using std::chrono::steady_clock;

    // base constructor
    ApiHandler::ApiHandler( const ApiSetting& setting ) noexcept
        : setting ( setting )
    {
        std::call_once( init_flag, []() {
            RestClient::init();
        } );

        const std::string api_conn_url = "http://" + setting.api_service_ip + ":" + std::to_string( setting.api_service_port );
        this->conn = std::make_unique<RestClient::Connection>( api_conn_url );
        this->conn->SetTimeout( 3 );
    }

    std::tuple<API_RSP_CODE, nlohmann::json> ApiHandler::GET_or_throw_if_timeout( std::string_view url, int timeoutSeconds )
    {
        std::unique_lock<std::mutex> lck { this->mtx };
        auto resp = this->conn->GET_or_throw_if_timeout( std::string { url }, timeoutSeconds );
        lck.unlock();

        if( ApiHandler::IsKnownResponseCode( resp.code ) )
            return { resp.code, RestClient::get_json_from_resp_body( resp.body ) };
        else
            return { resp.code, nlohmann::json::object() };
    }

    std::tuple<API_RSP_CODE, nlohmann::json> ApiHandler::GET_or_throw_if_timeout( std::string_view url, std::chrono::seconds timeoutDuration )
    {
        std::unique_lock<std::mutex> lck { this->mtx };
        auto resp = this->conn->GET_or_throw_if_timeout( std::string { url }, timeoutDuration );
        lck.unlock();

        if( ApiHandler::IsKnownResponseCode( resp.code ) )
            return { resp.code, RestClient::get_json_from_resp_body( resp.body ) };
        else
            return { resp.code, nlohmann::json::object() };
    }

    std::tuple<API_RSP_CODE, nlohmann::json> ApiHandler::GET_or_throw_if_timeout( std::string_view url, time_point<steady_clock> timeoutTime )
    {
        std::unique_lock<std::mutex> lck { this->mtx };
        auto resp = this->conn->GET_or_throw_if_timeout( std::string { url }, timeoutTime );
        lck.unlock();

        if( ApiHandler::IsKnownResponseCode( resp.code ) )
            return { resp.code, RestClient::get_json_from_resp_body( resp.body ) };
        else
            return { resp.code, nlohmann::json::object() };
    }

    bool ApiHandler::IsAlive( void )
    {
        return this->conn->is_rest_api_alive();
    }

    bool ApiHandler::IsGoodResponse( const API_RSP_CODE code ) noexcept
    {
        return code == 200;
    }

    bool ApiHandler::IsKnownResponseCode( const API_RSP_CODE code ) noexcept
    {
        return code >= 200 && code <= 700;
    }

    void ApiHandler::SetTimeout( const int to )
    {
        this->conn->SetTimeout( to );
    }

    std::string ApiHandler::GetMyLocalIp( void )
    {
        return this->setting.my_local_ip;
    }

    InterProtocolFunc ApiHandler::BuildCallback_GET( InterProtocolInputChecker checker, UrlBuilderFunc url_builder )
    {
        return std::bind( &ApiHandler::_BC_GET_Internal, this, std::placeholders::_1, checker, url_builder );
    }

    std::optional<nlohmann::json> ApiHandler::_BC_GET_Internal( const nlohmann::json& req_data, InterProtocolInputChecker checker, UrlBuilderFunc url_builder )
    {
        if( checker( req_data ) == false )
            return std::nullopt;

        const std::string req = url_builder( req_data );
        if( req.empty() )
            return std::nullopt;

        // GET_or_throw_if_timeout 은 더 이상 throw 하지 않음 (timeout 시 code <= 0).
        const auto [ code, body ] = this->GET_or_throw_if_timeout( req, 3 );
        if( ApiHandler::IsKnownResponseCode(code) == false )
            return std::nullopt;
        return body;
    }

    std::string API::_make_api_url_basic( const API::URL enum_tag ) noexcept
    {
        using MGEN::API::URL;

        switch( enum_tag ){
        case URL::NOT_SET:                                  return std::string { "" };
        // REST API endpoints (general)
        case URL::API_GET_SERVER_BY_IP:                    return std::string { "/server?ip=" };
        case URL::API_GET_SERVER_BY_ID:                    return std::string { "/server?id=" };
        case URL::API_GET_CLUSTER:                         return std::string { "/cluster?server_id=" };
        case URL::API_GET_EXCEPTION_CAMERA:                return std::string { "/exception?camera_id=" };
        case URL::API_GET_SCHEDULE:                        return std::string { "/schedule?camera_id=" };
        case URL::API_GET_CAMERA:                          return std::string { "/camera/media?camera_id=" };
        //
        default:                                            return std::string { "" };
        }
    }

    std::string API::_make_api_url_param( const std::vector<std::string>& values ) noexcept
    {
        std::string str_params {};
        for( size_t i = 0; i < values.size(); ++i ) {
            if( i != 0 ) str_params += ",";
            str_params += values[i];
        }
        return str_params;
    }

    std::string API::_make_api_url_param( const std::vector<int>& values ) noexcept
    {
        std::string str_params {};
        for( size_t i = 0; i < values.size(); ++i ) {
            if( i != 0 ) str_params += ",";
            str_params += std::to_string(values[i]);
        }
        return str_params;
    }

    std::string API::_make_api_url_param( const std::set<int>& set_values ) noexcept
    {
        std::vector<int> values {};
        values.insert( values.end(), set_values.begin(), set_values.end() );

        std::string str_params {};
        for( size_t i = 0; i < values.size(); ++i ) {
            if( i != 0 ) str_params += ",";
            str_params += std::to_string(values[i]);
        }
        return str_params;
    }

    std::string API::_make_api_url_param( const std::string& single_value ) noexcept
    {
        return single_value;
    }

    std::string API::_make_api_url_param( std::string_view single_value ) noexcept
    {
        return std::string { single_value };
    }

    std::string API::_make_api_url_param( const int& single_value ) noexcept
    {
        return std::to_string( single_value );
    }
};
