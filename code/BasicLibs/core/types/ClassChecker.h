#pragma once

#include "InferObject.h"
#include "string_utils.h"
#include "EngineProfile.h"
#include "MgenLogger.h"
#include "file_utils.h"

#include <functional>
#include <fstream>
#include <string>
#include <string_view>
#include <unordered_map>
// P3 (2026-05-14): vendored nlohmann (BasicLibs/core/parser/json/json.hpp) 사용.
// 시스템 <nlohmann/json.hpp> 와 혼용하면 json_abi_v3_x_x 네임스페이스 충돌 발생.
#include "json/json.hpp"

namespace MGEN
{
    using ClassTagName = std::string;
    using ClassIntID   = int;

    using ClassEqualChecker      = std::function<bool(const ClassIntID, const ClassTagName&)>;
    using ClassIDToNameConvertor = std::function<std::string(const ClassIntID)>;

    constexpr auto CLASS_TAG_NAME_NOT_SET = "NONE";

    class ClassChecker
    {
    public:
        explicit ClassChecker( const EngineProfile& profile ) noexcept
            // P3 (2026-05-14): YAML→JSON 마이그레이션. 변수명 _yml_path_ 는 EngineProfile API 호환성을 위해 유지.
            // 실제 파일은 .json 이며 EngineSettings.json 의 ClassesPath 에서 결정.
            : engine_id_       ( profile.GetProfileUUID() )
            , parse_yaml_path_ ( profile.GetClassesYmlPath() )
        {
            bool success = this->Initialize();
            assert( success );
        }

        bool Initialize( void )
        {
            if( IsValidFile( this->parse_yaml_path_ ) == false ) {
                MLOG_ERROR("ClassChecker::Initialize() failed, classes file ( %s ) is invalid",
                    this->parse_yaml_path_.c_str() );
                return false;
            }

            // P3: nlohmann::json 으로 파싱. 외부 라이브러리 throw 는 catch(...) 로 흡수
            // (CLAUDE.md "External library protection: catch(...) to absorb external throws").
            try {
                std::ifstream ifs { this->parse_yaml_path_ };
                if( !ifs ) {
                    MLOG_ERROR("ClassChecker::Initialize() failed, cannot open ( %s )",
                        this->parse_yaml_path_.c_str() );
                    return false;
                }

                nlohmann::json root;
                ifs >> root;

                if( !root.contains("classify_activate") ){
                    MLOG_ERROR("ClassChecker::Initialize() failed, classes file ( %s ) 'key = classify_activate' is invalid",
                        this->parse_yaml_path_.c_str() );
                    return false;
                }
                classify_activate_ = root["classify_activate"].get<bool>();

                if( this->classify_activate_ == false )
                    return true;

                if( !root.contains("num_class") ){
                    MLOG_ERROR("ClassChecker::Initialize() failed, classes file ( %s ) 'key = num_class' is invalid",
                        this->parse_yaml_path_.c_str() );
                    return false;
                }
                this->class_num_ = root["num_class"].get<int>();

                if( !root.contains("names") || !root["names"].is_object() ) {
                    MLOG_ERROR("ClassChecker::Initialize() failed, classes file ( %s ) 'key = names' is invalid",
                        this->parse_yaml_path_.c_str() );
                    return false;
                }

                // names: { "0": "person", "1": "bicycle", ... } 형식. key 는 문자열로 stoi 변환.
                for( auto it = root["names"].begin(); it != root["names"].end(); ++it ){

                    int id = std::stoi( it.key() );

                    std::string name  = it.value().get<std::string>();
                    std::string upper = MGEN::ToUpperCase(name);

                    this->id_to_name_[id]    = name;
                    this->name_to_id_[upper] = id;
                }
            }
            catch ( ... ) {
                MLOG_ERROR("ClassChecker::Initialize() failed, exception parsing classes file ( %s )",
                    this->parse_yaml_path_.c_str() );
                return false;
            }
            return true;
        }

        bool IsEnableClassify() const
        {
            return this->classify_activate_;
        }

        int GetClassNum( void ) const
        {
            return this->class_num_;
        }

        ClassEqualChecker GetClassEqualChecker( void ) const
        {
            return [this]( ClassIntID id, const ClassTagName& tag ) -> bool {
                std::string upper = MGEN::ToUpperCase(tag);
                auto it = name_to_id_.find(upper);
                return (it != name_to_id_.end() && it->second == id);
            };
        }

        ClassIDToNameConvertor GetClassIDToNameConvertor( void ) const
        {
            return [this]( ClassIntID id ) -> std::string {
                auto it = id_to_name_.find(id);
                return (it != id_to_name_.end()) ? it->second : CLASS_TAG_NAME_NOT_SET;
            };
        }

        InferEngineID GetInferEngineID( void ) const
        {
            return this->engine_id_;
        }

    private:
        const InferEngineID engine_id_;
        const std::string   parse_yaml_path_;

        std::unordered_map<ClassIntID, ClassTagName> id_to_name_;
        std::unordered_map<ClassTagName, ClassIntID> name_to_id_;

        bool classify_activate_ = false;
        int  class_num_ = 0;
    };

} // namespace MGEN
