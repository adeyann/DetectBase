#include "EngineProfile.h"
#include "MgenLogger.h"
#include "file_utils.h"

namespace MGEN
{
    EngineProfile::EngineProfile( const EngineProfileConfig& config ) noexcept
        : _uuid                      ( config.uuid )
        , _profile_name              ( config.profile_name )
        , _engine_file_path          ( config.engine_file_path )
        , _classes_yml_path          ( config.classes_yml_path )
        , _model_major_type          ( config.model_major_type )
        , _model_minor_type          ( config.model_minor_type )
        , _model_magic_type          ( config.model_magic_type )
        , _priority                  ( config.priority )
        , _inference_weight          ( config.inference_weight )
        , _input_size_mutable        ( config.input_size_mutable )
        , _input_image_w             ( config.input_image_w )
        , _input_image_h             ( config.input_image_h )
        , _inference_batch_size      ( config.inference_batch_size )
        , _opt_type                  ( config.opt_type )
        , _input_layer_binding_name  ( config.input_layer_binding_name )
        , _output_layer_binding_name ( config.output_layer_binding_name )
        , _confidence_threshold      ( config.confidence_threshold )
        , _nms_threshold             ( config.nms_threshold)
    {
        //
    }

    EngineProfile::EngineProfile( const EngineProfile& profile ) noexcept
        : _uuid                      ( profile._uuid )
        , _profile_name              ( profile._profile_name )
        , _engine_file_path          ( profile._engine_file_path )
        , _classes_yml_path          ( profile._classes_yml_path )
        , _model_major_type          ( profile._model_major_type )
        , _model_minor_type          ( profile._model_minor_type )
        , _model_magic_type          ( profile._model_magic_type )
        , _priority                  ( profile._priority )
        , _inference_weight          ( profile._inference_weight )
        , _input_size_mutable        ( profile._input_size_mutable )
        , _input_image_w             ( profile._input_image_w )
        , _input_image_h             ( profile._input_image_h )
        , _inference_batch_size      ( profile._inference_batch_size )
        , _opt_type                  ( profile._opt_type )
        , _input_layer_binding_name  ( profile._input_layer_binding_name )
        , _output_layer_binding_name ( profile._output_layer_binding_name )
        , _confidence_threshold      ( profile._confidence_threshold )
        , _nms_threshold             ( profile._nms_threshold)
    {
        //
    }

    std::string EngineProfile::ToString(ModelMajorType type)
    {
        switch (type) {
            case ModelMajorType::None:           return "None(Don't care)";
            case ModelMajorType::Detection:      return "Detection";
        }
        return "Unknown";
    }

    std::string EngineProfile::ToString(ModelMinorType type)
    {
        switch (type) {
            case ModelMinorType::None:     return "None(Don't care)";
            case ModelMinorType::YoloV5:   return "YoloV5";
            case ModelMinorType::YoloV7:   return "YoloV7";
            case ModelMinorType::YoloV8:   return "YoloV8";
            case ModelMinorType::YoloV10:  return "YoloV10";
        }
        return "Unknown";
    }

    std::string EngineProfile::ToString(ModelOptimizeType type)
    {
        switch (type) {
            case ModelOptimizeType::None:            return "None(Don't care)";
            case ModelOptimizeType::Torch_Onnx_RKNN: return "Torch_Onnx_RKNN";
        }
        return "Unknown";
    }

    void EngineProfile::ShowProfileInfo( void ) const noexcept
    {
        char summary_buffer[1024] = { 0, };
        // NEW-9: _uuid (InferEngineID = unsigned int) + _inference_batch_size (unsigned int) → %u 사용.
        snprintf( summary_buffer, sizeof(summary_buffer),
            "   - [%u] %-25s [ %-8s | Batch_%u | %-15s ] : %s",
            _uuid,
            _profile_name.c_str(),
            EngineProfile::ToString(_model_minor_type).c_str(),
            _inference_batch_size,
            _model_magic_type.c_str(),
            GetFileBaseName(_engine_file_path).c_str()
        );
        MLOG_INFO(  "%s", summary_buffer);
    }

    InferRequestRequirements CreateRequirementsFromSingle(const InferRequestRequireOneType& single_req)
    {
        // 1. 기본값(모든 필드가 DontCare)으로 InferRequestRequirements 객체 생성
        InferRequestRequirements requirements; // 구조체 정의 시 기본값 할당됨

        // 2. std::visit를 사용하여 variant 내부 타입에 따라 처리
        std::visit(
            // requirements 객체를 참조로 캡처하여 수정할 수 있도록 함
            [&requirements](const auto& value) {
                // variant에 저장된 값의 실제 타입(const, & 제거)을 얻음
                using ValueType = std::decay_t<decltype(value)>;

                // 3. 타입에 맞춰 requirements 객체의 해당 필드 업데이트
                if constexpr (std::is_same_v<ValueType, ModelMagicType>) { // ModelMagicType == std::string
                    requirements.magic_type = value;
                } else if constexpr (std::is_same_v<ValueType, InferEngineID>) {
                    requirements.model_uuid = value;
                }
            },
            single_req // 방문할 variant 객체
        );

        // 4. 필드가 업데이트된 requirements 객체 반환
        return requirements;
    }

    static bool IsRequireAtLeastOne( const InferRequestRequirements& require )
    {
        return (require.major_type != ModelMajorTypeDontCare) ||
               (require.minor_type != ModelMinorTypeDontCare) ||
               (require.magic_type != ModelMagicTypeDontCare) || // ModelMagicTypeDontCare가 const char* 라면 직접 비교 가능
               (require.model_uuid != InferModelUUIDDontCare);
    }

    std::vector<InferEngineID> SearchEngineUUID( const EngineProfileMap& profiles, const InferRequestRequirements& requirements )
    {
        // 1. 임시 저장소: (우선순위, 엔진ID) 쌍의 벡터
        std::vector<std::pair<unsigned int, InferEngineID>> candidate_pairs;

        // 요구사항이 하나도 없으면 빈 벡터 반환
        if( !IsRequireAtLeastOne( requirements ) ) {
            // 비어있는 벡터 반환
            return {};
        }

        // --- 단일 순회 필터링 시작 ---
        for( const auto& [ candidate_uuid, profile ] : profiles ) {
            bool match_found = false; // 해당 프로필이 요구사항 중 하나라도 만족했는지 여부

            // (요구사항 필터링 로직은 이전과 동일)
            if (requirements.major_type != ModelMajorTypeDontCare &&
                requirements.major_type == profile.GetModelMajorType()) {
                match_found = true;
            }
            if (!match_found &&
                requirements.minor_type != ModelMinorTypeDontCare &&
                requirements.minor_type == profile.GetModelMinorType()) {
                match_found = true;
            }
             if (!match_found &&
                requirements.magic_type != ModelMagicTypeDontCare &&
                requirements.magic_type == profile.GetModelMagicType()) {
                match_found = true;
            }
             if (!match_found &&
                requirements.model_uuid != InferModelUUIDDontCare &&
                requirements.model_uuid == candidate_uuid) {
                match_found = true;
            }

            // 요구사항을 만족하는 경우, (우선순위, UUID) 쌍을 임시 벡터에 추가
            if (match_found) {
                candidate_pairs.emplace_back(profile.GetPriority(), candidate_uuid);
            }
        }
        // --- 단일 순회 필터링 끝 ---

        // 3. 우선순위(priority) 기준으로 후보 쌍 정렬
        // std::pair는 기본적으로 first 멤버(여기서는 priority)를 기준으로 오름차순 정렬됨.
        // 따라서 priority 값이 낮은 것이 앞에 오게 됨 (우선순위가 높은 순).
        std::sort(candidate_pairs.begin(), candidate_pairs.end());

        // 4. 정렬된 UUID만 추출하여 최종 결과 벡터 생성
        std::vector<InferEngineID> sorted_candidates;
        sorted_candidates.reserve(candidate_pairs.size()); // 메모리 미리 할당
        for (const auto& pair : candidate_pairs) {
            sorted_candidates.push_back(pair.second); // UUID (pair의 두 번째 요소)만 저장
        }

        // 5. 우선순위에 따라 정렬된 UUID 벡터 반환
        return sorted_candidates;
    }

    std::vector<InferEngineID> SearchEngineUUID( const EngineProfileMap& profiles, const InferRequestRequireOneType& requirements )
    {
        return SearchEngineUUID( profiles, CreateRequirementsFromSingle( requirements ) );
    }

    void ShowEngineProfiles( const std::vector<MGEN::EngineProfile>& engine_profiles )
    {
        for( const auto& p : engine_profiles )
            p.ShowProfileInfo();
    }

} // namespace MGEN
