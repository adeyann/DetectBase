#ifndef _MGEN_ENGINE_PROFILE_PARSER_H_
#define _MGEN_ENGINE_PROFILE_PARSER_H_

#include "EngineProfile.h"
#include "InferObject.h"

#include <vector>
#include <string>
#include <atomic>
#include <optional>

namespace MGEN
{
    class EngineProfileParser
    {
    public:
        using ErrMsg = std::string;

        // EngineSetting.json 및 이하 파일들에 대한 유효성 검사
        // return:
        //   if all ok, return std::nullopt
        //   if something error, return std::optional<std::string> with error message string
        static std::optional<EngineProfileParser::ErrMsg> CheckParsable(const std::string& engine_settings_json_path);

        // JSON 파일 경로를 파싱하여 EngineProfile 객체 목록 반환.
        // 실패 시 std::nullopt — 호출자가 검사 후 처리 (throw 사용 안 함).
        static std::optional<std::vector<EngineProfile>> Parse(const std::string& engine_settings_json_path);

    private:
        // 문자열을 enum으로 변환하는 유틸리티 함수들
        static ModelMajorType      ParseModelMajorType   (const std::string& type);
        static ModelMinorType      ParseModelMinorType   (const std::string& type);
        static ModelOptimizeType   ParseModelOptimizeType(const std::string& type);

        // 고유 ID를 생성하는 내부 함수 (외부에서 접근 불가)
        static InferEngineID GenerateUniqueID( void ) noexcept;

        // 고유 ID를 위한 자동 증가 변수 (static)
        static std::atomic<InferEngineID> unique_id_counter; // 고유 ID 카운터
    };
}

#endif // _MGEN_ENGINE_PROFILE_PARSER_H_
