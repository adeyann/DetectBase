#pragma once

#include "rest_client.h"
#include "rest_connection.h"

#include <chrono>
#include <string>

#include "json/json_fwd.hpp"

namespace RestClient
{
	constexpr int GET_STATUS_OK = 200;

    // JSON 변환 함수
    nlohmann::json get_json_from_resp_body( Response& resp );
    nlohmann::json get_json_from_resp_body( const std::string& resp_body );
};
