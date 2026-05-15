#include "EngineProfileParser.h"
#include "file_utils.h"
#include "MgenLogger.h"
#include "string_utils.h"

#include <fstream>
#include "json/json.hpp"

using json = nlohmann::json;

std::atomic<MGEN::InferEngineID> MGEN::EngineProfileParser::unique_id_counter {MGEN::INFER_ENGINE_ID_INITIAL};

namespace MGEN
{
    // 내부 경로 치환 유틸
    static std::string ResolvePath(const std::string& raw_path, const std::string& detectbase_root)
    {
        const std::string keyword = "DETECTBASE_ENGINE_FILES_ROOT_PATH/";
        if (raw_path.find(keyword) == 0) {
            return MGEN::ConcatPath(detectbase_root, raw_path.substr(keyword.length()));
        }
        return raw_path;
    }

    std::optional<EngineProfileParser::ErrMsg> EngineProfileParser::CheckParsable( const std::string& engine_settings_json_path )
    {
        const std::string& json_path = engine_settings_json_path;

        if( IsValidFile( json_path ) == false ){
            return "CheckParsable(), EngineSettings 파일이 존재하지 않거나 잘못된 경로입니다: " + json_path;
        }

        std::ifstream settings_ifs { json_path };
        if( !settings_ifs ){
            return "CheckParsable(), EngineSettings 파일을 열 수 없습니다: " + json_path;
        }

        json settings_json;
        try {
            settings_ifs >> settings_json;
        }
        catch ( const std::exception& e ){
            return std::string("CheckParsable(), JSON 파싱 오류: ") + e.what();
        }

        const std::string detectbase_root = settings_json.value("DETECTBASE_ENGINE_FILES_ROOT_PATH", "");
        if( detectbase_root.empty() ){
            return "CheckParsable(), DETECTBASE_ENGINE_FILES_ROOT_PATH 키가 누락되었거나 값이 비어 있습니다.";
        }

        if( !settings_json.contains("Engines") || !settings_json["Engines"].is_array() ){
            return "CheckParsable(), 'Engines' 배열이 존재하지 않거나 형식이 올바르지 않습니다.";
        }

        const auto& engines = settings_json["Engines"];
        for( const auto& engine_entry : engines )
        {
            if( engine_entry.value("Enable", false) == false )
                continue;

            const std::string profile_path = ResolvePath( engine_entry.value("ProfilePath", ""),    detectbase_root );
            const std::string classes_path = ResolvePath( engine_entry.value("ClassesPath", ""),    detectbase_root );
            const std::string engine_path  = ResolvePath( engine_entry.value("EngineFilePath", ""), detectbase_root );

            if( !IsValidFile(GetAbsolutePath(profile_path)) )
                return "CheckParsable(), ProfilePath 경로의 파일이 존재하지 않습니다: " + profile_path;

            if( !IsValidFile(GetAbsolutePath(engine_path)) )
                return "CheckParsable(), EngineFilePath 경로의 파일이 존재하지 않습니다: " + engine_path;

            if( !IsValidFile(GetAbsolutePath(classes_path)) )
                return "CheckParsable(), ClassesPath 경로의 파일이 존재하지 않습니다: " + classes_path;
        }

        return std::nullopt; // 모두 통과
    }

    InferEngineID EngineProfileParser::GenerateUniqueID( void )  noexcept
    {
        return unique_id_counter++;
    }

    static void CheckKeyExist( const std::vector<std::string>& check_list, const json& json, const std::string& json_path )
    {
        for( const auto& check : check_list ) {
            if( json.contains( check ) == false ) {
                MLOG_WARN("( %s ) ===> JSON have not key [ %s ]", json_path.c_str(), check.c_str() );
            }
        }
    }

    std::optional<std::vector<EngineProfile>> EngineProfileParser::Parse( const std::string& engine_settings_json_path )
    {
        const auto& json_path = engine_settings_json_path;
        if( EngineProfileParser::CheckParsable( json_path ) != std::nullopt ) {
            MLOG_ERROR( "EngineProfileParser::Parse - Target json file not parsable: %s", json_path.c_str() );
            return std::nullopt;
        }

        std::vector<EngineProfile> result {};

        std::ifstream settings_ifs { json_path };
        if( !settings_ifs ){
            MLOG_ERROR( "EngineProfileParser::Parse - Failed to open EngineSettings.json: %s", json_path.c_str() );
            return std::nullopt;
        }

        json settings_json;
        settings_ifs >> settings_json;

        const std::string detectbase_root = settings_json.value("DETECTBASE_ENGINE_FILES_ROOT_PATH", "");
        if( !settings_json.contains("Engines") || !settings_json["Engines"].is_array() ){
            MLOG_ERROR( "EngineProfileParser::Parse - Invalid or missing 'Engines' array in EngineSettings.json" );
            return std::nullopt;
        }

        std::vector<std::string> key_list = {
            "ProfileName",
            "ModelMajorType",
            "ModelMinorType",
            "ModelMagicType",
            "Priority",
            "InferenceBatchSize",
            "InferenceWeight",
            "InputSizeMutable",
            "InputImageWidth",
            "InputImageHeight",
            "ModelOptimizeType",
            "InputLayerBindingName",
            "OutputLayerBindingName",
            "ConfidenceThreshold",
            "NmsThreshold"
        };

        const auto& engines = settings_json["Engines"];
        for( const auto& engine_entry : engines )
        {
            if (!engine_entry.value("Enable", false))
                continue;

            const std::string profile_path = ResolvePath( engine_entry.value("ProfilePath", ""),    detectbase_root );
            const std::string classes_path = ResolvePath( engine_entry.value("ClassesPath", ""),    detectbase_root );
            const std::string engine_path  = ResolvePath( engine_entry.value("EngineFilePath", ""), detectbase_root );

            std::ifstream profile_ifs { profile_path };
            if( !profile_ifs ){
                MLOG_ERROR( "EngineProfileParser::Parse - Failed to open profile JSON: %s", profile_path.c_str() );
                return std::nullopt;
            }

            json profile_json;
            profile_ifs >> profile_json;

            CheckKeyExist( key_list, profile_json, profile_path );

            EngineProfileConfig config;
            config.uuid                      = EngineProfileParser::GenerateUniqueID();
            config.profile_name              = profile_json.value("ProfileName", "");
            config.engine_file_path          = engine_path;
            config.classes_yml_path          = classes_path;
            config.model_major_type          = ParseModelMajorType(profile_json.value("ModelMajorType", ""));
            config.model_minor_type          = ParseModelMinorType(profile_json.value("ModelMinorType", ""));
            config.model_magic_type          = profile_json.value("ModelMagicType", "");
            config.priority                  = profile_json.value("Priority", 99);
            config.inference_weight          = profile_json.value("InferenceWeight", 100);
            config.input_size_mutable        = profile_json.value("InputSizeMutable", false);
            config.input_image_w             = profile_json.value("InputImageWidth", 640);
            config.input_image_h             = profile_json.value("InputImageHeight", 640);
            config.inference_batch_size      = profile_json.value("InferenceBatchSize", 1);
            config.opt_type                  = ParseModelOptimizeType(profile_json.value("ModelOptimizeType", "None"));
            config.input_layer_binding_name  = profile_json.value("InputLayerBindingName", "images");
            config.output_layer_binding_name = profile_json.value("OutputLayerBindingName", "output");
            config.confidence_threshold      = profile_json.value("ConfidenceThreshold", 0.45f);
            config.nms_threshold             = profile_json.value("NmsThreshold", 0.5f);

            result.emplace_back( config );
        }
        return result;
    }

    // 문자열 -> enum 변환 유틸들
    ModelMajorType EngineProfileParser::ParseModelMajorType(const std::string& type)
    {
        const std::string upper_type = MGEN::ToUpperCase( type );
        if( upper_type == MGEN::ToUpperCase( "Detection"      ) ) return ModelMajorType::Detection;
        return ModelMajorType::None;
    }

    ModelMinorType EngineProfileParser::ParseModelMinorType(const std::string& type)
    {
        const std::string upper_type = MGEN::ToUpperCase( type );
        if( upper_type == MGEN::ToUpperCase( "YoloV5"   ) ) return ModelMinorType::YoloV5;
        if( upper_type == MGEN::ToUpperCase( "YoloV7"   ) ) return ModelMinorType::YoloV7;
        if( upper_type == MGEN::ToUpperCase( "YoloV8"   ) ) return ModelMinorType::YoloV8;
        if( upper_type == MGEN::ToUpperCase( "YoloV10"  ) ) return ModelMinorType::YoloV10;
        return ModelMinorType::None;
    }

    ModelOptimizeType EngineProfileParser::ParseModelOptimizeType(const std::string& type)
    {
        const std::string upper_type = MGEN::ToUpperCase( type );
        if( upper_type == MGEN::ToUpperCase( "Torch_Onnx_RKNN" ) ) return ModelOptimizeType::Torch_Onnx_RKNN;
        return ModelOptimizeType::None;
    }
}
