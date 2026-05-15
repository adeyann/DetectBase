#pragma once

// From Cmake
#include "ProgramConfig.h"

// MGEN
#include "file_utils.h"

// STL
#include <string_view>
#include <string>

/*-*-* [ Initialize Local File Setting ] -*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*/
constexpr std::string_view APPLICATION_NAME           = "DetectBase";
constexpr std::string_view APPLICATION_LOG_PATH       = "/DetectBase/logs/DetectBase.log";
constexpr std::string_view APPLICATION_SETTINGS_DIR   = "/DetectBase/settings/"; // DetectBase 서비스 세팅 파일 저장 폴더 경로
constexpr std::string_view NETWORK_SETTINGS_JSON_PATH = "NetworkSettings.json";
constexpr std::string_view ENGINES_SETTINGS_JSON_PATH = "EngineSettings.json";
/*-*-* [ Initialize Local File Setting ] -*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*/

namespace MGEN
{
    inline std::string GetApplicationVersion( void ) noexcept
    {
        std::string str_version = "";
        str_version
            += std::to_string( DETECTBASE_VERSION_MAJOR ) + "."
            +  std::to_string( DETECTBASE_VERSION_MINOR ) + "."
            +  std::to_string( DETECTBASE_VERSION_PATCH );
        return str_version;
    };

    inline std::string GetNetworkProfileJsonPath( void )
    {
        return ConcatPath(
            std::string { APPLICATION_SETTINGS_DIR   },
            std::string { NETWORK_SETTINGS_JSON_PATH }
        );
    }

    inline std::string GetEngineProfilesJsonPath( void )
    {
        return ConcatPath(
            std::string { APPLICATION_SETTINGS_DIR   },
            std::string { ENGINES_SETTINGS_JSON_PATH }
        );
    }
};