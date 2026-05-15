#ifndef _MGEN_ENGINE_PROFILE_H_
#define _MGEN_ENGINE_PROFILE_H_

#include "InferObject.h"

#include <string>
#include <unordered_map>
#include <set>
#include <variant>

namespace MGEN
{
    // ModelMajorType, ModelMinorType 모두 같을 때 최종 구별 식별자 문자열
    using ModelMagicType = std::string;

    // 모델의 주요 카테고리
    enum class ModelMajorType {
        None,
        Detection,
    };

    // 세부적인 모델 종류 (Yolo 버전별 분기 보존)
    enum class ModelMinorType {
        None,
        YoloV5,
        YoloV7,
        YoloV8,
        YoloV10,
    };

    // 모델 최적화 방식 (NPU 환경 전용)
    enum class ModelOptimizeType {
        None,
        Torch_Onnx_RKNN,
    };

    struct EngineProfileConfig {
        InferEngineID             uuid;
        std::string               profile_name;
        std::string               engine_file_path;
        std::string               classes_yml_path;
        MGEN::ModelMajorType      model_major_type;
        MGEN::ModelMinorType      model_minor_type;
        MGEN::ModelMagicType      model_magic_type;
        unsigned int              priority;
        unsigned int              inference_weight;
        bool                      input_size_mutable;
        unsigned int              input_image_w;
        unsigned int              input_image_h;
        unsigned int              inference_batch_size;
        MGEN::ModelOptimizeType   opt_type;
        std::string               input_layer_binding_name;
        std::string               output_layer_binding_name;
        float                     confidence_threshold;
        float                     nms_threshold;
    };

    class EngineProfile
    {
    public:
        // Default constructor delete
        EngineProfile() = delete;

        // Base constructor
        explicit EngineProfile( const EngineProfileConfig& config ) noexcept;

        // copy constructor
        explicit EngineProfile( const EngineProfile& profile ) noexcept;

        // Destructor
        ~EngineProfile() = default;

        // Getters
        [[nodiscard]] inline InferEngineID             GetProfileUUID()            const noexcept { return this->_uuid; }
        [[nodiscard]] inline std::string               GetProfileName()            const noexcept { return this->_profile_name; }
        [[nodiscard]] inline std::string               GetEngineFileName()         const noexcept { return this->_engine_file_path; }
        [[nodiscard]] inline std::string               GetClassesYmlPath()         const noexcept { return this->_classes_yml_path; }
        [[nodiscard]] inline MGEN::ModelMajorType      GetModelMajorType()         const noexcept { return this->_model_major_type; }
        [[nodiscard]] inline MGEN::ModelMinorType      GetModelMinorType()         const noexcept { return this->_model_minor_type; }
        [[nodiscard]] inline MGEN::ModelMagicType      GetModelMagicType()         const noexcept { return this->_model_magic_type; }
        [[nodiscard]] inline unsigned int              GetPriority()               const noexcept { return this->_priority; }
        [[nodiscard]] inline unsigned int              GetModelInferenceWeight()   const noexcept { return this->_inference_weight; }
        [[nodiscard]] inline bool                      GetInputSizeMutable()       const noexcept { return this->_input_size_mutable; }
        [[nodiscard]] inline unsigned int              GetInputImageWidth()        const noexcept { return this->_input_image_w; }
        [[nodiscard]] inline unsigned int              GetInputImageHeight()       const noexcept { return this->_input_image_h; }
        [[nodiscard]] inline unsigned int              GetInferenceBatchSize()     const noexcept { return this->_inference_batch_size; }
        [[nodiscard]] inline MGEN::ModelOptimizeType   GetModelOptimizeType()      const noexcept { return this->_opt_type; }
        [[nodiscard]] inline std::string               GetInputLayerBindingName()  const noexcept { return this->_input_layer_binding_name; }
        [[nodiscard]] inline std::string               GetOutputLayerBindingName() const noexcept { return this->_output_layer_binding_name; }
        [[nodiscard]] inline float                     GetConfidenceThreshold()    const noexcept { return this->_confidence_threshold; }
        [[nodiscard]] inline float                     GetNmsThreshold()           const noexcept { return this->_nms_threshold; }

        static std::string ToString( ModelMajorType type );
        static std::string ToString( ModelMinorType type );
        static std::string ToString( ModelOptimizeType type );

        void ShowProfileInfo( void ) const noexcept;

    private:
        const InferEngineID             _uuid;                      // 해당 프로파일에 대한 UUID 식별자
        const std::string               _profile_name;              // 해당 프로파일을 지정하는 Unique(PK) 이름값
        const std::string               _engine_file_path;          // 해당 프로파일에 할당된 엔진의 실제 파일 이름(경로)
        const std::string               _classes_yml_path;          // 해당 프로피일과 매핑되는 엔진의 클래스 정의 yaml 파일 경로
        const MGEN::ModelMajorType      _model_major_type;          // Detection, Attribute, Segmentation 등 엔진 모델의 주요 분류
        const MGEN::ModelMinorType      _model_minor_type;          // Yolo, Resnet 등 엔진 모델명
        const MGEN::ModelMagicType      _model_magic_type;          // ModelMajorType, ModelMinorType 모두 같을 때 최종 구별 식별자 문자열 (Not PK)
        const unsigned int              _priority;                  // 동일 분류 단계에서의 우선 순위. 숫자가 낮을수록 우선순위 높음(1~99 사이의 값)
        const unsigned int              _inference_weight;          // 엔진 속도 가중치, 1 대비 N은 대략 1/N 속도라고 가정
        const bool                      _input_size_mutable;        // 추론 입력 이미지 사이즈의 변동 가능 여부. Crop Image 기반의 추론 시에만 true
        const unsigned int              _input_image_w;             // 이미지 추론 시 해당 엔진의 input image size : 너비값
        const unsigned int              _input_image_h;             // 이미지 추론 시 해당 엔진의 input image size : 높이값
        const unsigned int              _inference_batch_size;      // 해당 엔진 학습 or 최적화 시 설정해 둔 inference batch size
        const MGEN::ModelOptimizeType   _opt_type;                  // 엔진 최적화 방식
        const std::string               _input_layer_binding_name;  // 엔진 모델 InputLayer에 binding된 name값
        const std::string               _output_layer_binding_name; // 엔진 모델 OutputLayer에 binding된 name값
        const float                     _confidence_threshold;      // 엔진 모델 Inference confidence(score) 최소 검출 수치
        const float                     _nms_threshold;             // 엔진 모델 NMS 최소 검출 수치
    };

    using EngineProfileMap = std::unordered_map<InferEngineID, EngineProfile>;

    constexpr auto ModelMajorTypeDontCare = ModelMajorType::None;
    constexpr auto ModelMinorTypeDontCare = ModelMinorType::None;
    constexpr auto ModelMagicTypeDontCare = "DontCareMagic";
    constexpr auto InferModelUUIDDontCare = 0xffffff;

    struct InferRequestRequirements
    {
        MGEN::ModelMajorType major_type = ModelMajorTypeDontCare;
        MGEN::ModelMinorType minor_type = ModelMinorTypeDontCare;
        MGEN::ModelMagicType magic_type = std::string { ModelMagicTypeDontCare };
        MGEN::InferEngineID  model_uuid = InferModelUUIDDontCare;
    };
    using InferRequestRequireOneType = std::variant<
        ModelMagicType,     // Magic Type 요구사항 (std::string)
        InferEngineID       // 특정 Model UUID 요구사항
    >;

    /**
     * @brief 단일 요구사항 variant를 받아 InferRequestRequirements 구조체를 생성합니다.
     * @details variant에 담긴 타입에 해당하는 필드만 설정되고 나머지는 DontCare 값으로 초기화됩니다.
     * @param single_req 단일 요구사항을 담은 std::variant 객체.
     * @return 해당하는 필드가 설정된 InferRequestRequirements 객체.
     */
    InferRequestRequirements CreateRequirementsFromSingle( const InferRequestRequireOneType& single_req );

    /**
     * @brief 여러 요구사항을 기반으로 EngineProfileMap에서 일치하는 엔진 UUID들을 검색합니다.
     * @param profiles 검색 대상 EngineProfile 맵.
     * @param requirement 여러러 요구사항을 담고 있는 InferRequestRequirements 객체.
     * @return 요구사항과 일치하는 InferEngineID들의 std::vector.
     */
    std::vector<InferEngineID> SearchEngineUUID( const EngineProfileMap& profiles, const InferRequestRequirements& requirements );

    /**
     * @brief 단일 요구사항(variant)을 기반으로 EngineProfileMap에서 일치하는 엔진 UUID들을 검색합니다.
     * @param profiles 검색 대상 EngineProfile 맵.
     * @param requirement 단일 요구사항을 담고 있는 std::variant 객체.
     * @return 요구사항과 일치하는 InferEngineID들의 std::vector.
     */
    std::vector<InferEngineID> SearchEngineUUID( const EngineProfileMap& profiles, const InferRequestRequireOneType& requirements );

    // print helper
    void ShowEngineProfiles( const std::vector<MGEN::EngineProfile>& engine_profiles );

} // namespace MGEN

#endif // _MGEN_ENGINE_PROFILE_H_
