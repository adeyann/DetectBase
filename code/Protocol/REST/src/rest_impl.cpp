#include "rest_impl.h"
#include "MgenLogger.h"
#include "json/json.hpp"

#include <sstream>

static int GetIntSecondsFromTimePoint( const std::chrono::time_point<std::chrono::steady_clock>& timeoutTime )
{
	// 현재 시간과의 차이를 초(second) 단위로 변환
	auto duration = std::chrono::duration_cast<std::chrono::seconds>(timeoutTime.time_since_epoch());
	return duration.count();
}

// Timeout 발생 시 빈 Response (code = 28, CURLE_OPERATION_TIMEDOUT 매핑) 반환.
// 기존 std::async + future timeout 패턴은 cURL 자체 timeout 으로 대체 (F-3 cURL 누수 해결).
RestClient::Response RestClient::Connection::GET_or_throw_if_timeout( const std::string& url, int timeoutSeconds )
{
	auto prev_timeout = this->GetInfo().timeout;

	if( timeoutSeconds < prev_timeout || prev_timeout <= 0 )
		this->SetTimeout( timeoutSeconds );

	auto resp = this->get( url );

	this->SetTimeout( prev_timeout );

	if( resp.code <= 0 ){
		MLOG_ERROR("Connect REST API ( %s ), Timeout or transport error (code=%d).", url.c_str(), resp.code );
	}
	return resp;
}

RestClient::Response RestClient::Connection::GET_or_throw_if_timeout( const std::string& url, std::chrono::seconds timeoutDuration )
{
    return GET_or_throw_if_timeout( url, timeoutDuration.count() );
}

RestClient::Response RestClient::Connection::GET_or_throw_if_timeout( const std::string& url, std::chrono::time_point<std::chrono::steady_clock> timeoutTime )
{
    return GET_or_throw_if_timeout( url, GetIntSecondsFromTimePoint( timeoutTime ) );
}

// JSON 변환 함수 (외부 라이브러리 nlohmann::json 보호 패턴 — catch 보존)
nlohmann::json RestClient::get_json_from_resp_body( Response& resp )
{
    try {
        if( resp.body.empty() )
            return nlohmann::json::object();

        nlohmann::json res;
        std::stringstream(resp.body) >> res;

        return res;
    }
    catch( ... ) {
        return nlohmann::json::object();
    }
}

nlohmann::json RestClient::get_json_from_resp_body( const std::string& resp_body )
{
    try {
        if( resp_body.empty() )
            return nlohmann::json::object();

        nlohmann::json res;
        std::stringstream(resp_body) >> res;

        return res;
    }
    catch( ... ) {
        return nlohmann::json::object();
    }
}

bool RestClient::Connection::is_rest_api_alive()
{
    CURL* curl = curl_easy_init();
    if (!curl)
		return false;

	std::string url = this->GetInfo().baseUrl + "/";

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 1L);
    curl_easy_setopt(curl, CURLOPT_NOBODY, 1L); // HEAD 요청
    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    return (res == CURLE_OK);
}
