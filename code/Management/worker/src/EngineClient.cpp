/**
 * @file EngineClient.cpp
 * @brief EngineClient의 초기화, 요청 생성 및 클래스 매핑 로직을 구현합니다.
 */

#include "EngineClient.h" // 자신의 헤더 포함
#include "string_utils.h"  // ToUpperCase 함수 사용을 위해 포함
#include "MgenLogger.h"    // 로그 출력을 위해 포함

#include <algorithm> // std::move 사용을 위해 포함

namespace MGEN
{
    bool EngineClient::Init(
        const MGEN::Type::UnitID                                 my_unit_id,
        const std::shared_ptr<EngineLoadBalancer>&               lb,
        std::string_view                                         target_magic_name,
        const MGEN::Type::UnitID                                 magic_code,
        const std::vector<std::string>&                          target_class_names,
        std::function<bool( const EngineActiveContext& )>        condition
    )
    {
        if( lb == nullptr )
        {
            return false;
        }

        // 1. 엔진 ID 획득
        this->engine_id = lb->GetAvailableInferEngineID( std::string{ target_magic_name } );
        if( this->engine_id == INFER_ENGINE_ID_NOT_SET )
        {
            return false;
        }

        // 2. 엔진 프로파일 정보 획득
        auto profile_opt = lb->GetEngineProfileTargetInferEngineID( this->engine_id );
        if( profile_opt.has_value() == false )
        {
            return false;
        }

        const EngineProfile& profile = *profile_opt;

        this->magic_name        = std::string{ target_magic_name };
        this->input_width       = static_cast<int>( profile.GetInputImageWidth() );
        this->input_height      = static_cast<int>( profile.GetInputImageHeight() );
        this->task_type         = profile.GetModelMajorType();
        this->optimize_type     = profile.GetModelOptimizeType();
        this->is_active_condition = std::move( condition );

        // 3. 엔진 구독을 위한 Subscribe ID 생성 (Jump Offset 규칙 적용)
        this->subscribe_id = GetInferenceRequesterID(
            my_unit_id,
            profile.GetModelMajorType(),
            profile.GetModelMinorType(),
            magic_code
        );

        // 4. 추론 요청 메타데이터 템플릿 생성
        EngineStreamMetaData meta_data;
        meta_data.requester_unit_id     = this->subscribe_id;
        meta_data.requestee_engine_uuid = this->engine_id;
        meta_data.request_image_pixel_w = static_cast<size_t>( this->input_width );
        meta_data.request_image_pixel_h = static_cast<size_t>( this->input_height );
        meta_data.correlation_id        = 0;

        // 엔진별 요구 명세 생성
        auto req_spec = CreateRequirementsFromSingle( this->engine_id );

        // 템플릿 래퍼 구축 (이미지는 요청 시점에 바인딩)
        this->request_meta = std::make_shared<InputLayerWrapper>(
            InputLayerWrapper::Build( meta_data, req_spec, nullptr )
        );

        // 5. 대상 클래스 이름-ID 매핑 테이블 구축
        auto class_checker_inst = std::make_unique<ClassChecker>( profile );
        auto checker_func       = class_checker_inst->GetClassEqualChecker();
        size_t total_classes    = class_checker_inst->GetClassNum();

        for( const auto& name : target_class_names )
        {
            std::string upper_target = MGEN::ToUpperCase( name );

            for( size_t i = 0; i < total_classes; ++i )
            {
                if( checker_func( static_cast<int>( i ), upper_target ) == true )
                {
                    this->target_class_ids[upper_target] = static_cast<MGEN::InferClassID>( i );
                    break;
                }
            }
        }

        return true;
    }

    InputLayerWrapper EngineClient::BuildRequest( FrameFormattingContext* ctx, uint64_t correlation_id )
    {
        // 템플릿 메타데이터 복사
        InputLayerWrapper req = *this->request_meta;

        // 식별용 ID 부여
        req.meta_data.correlation_id = correlation_id;

        // 이미지 데이터 공유 (참조 카운트 기반 복사 방지)
        if( ctx != nullptr && ctx->IsReadyForInference() == true )
        {
            // 참조 카운트 기반으로 데이터 공유 (복사 발생 안 함)
            req.image_data = ctx->shared_input_blob;
        }
        else
        {
            // 데이터가 없는 상태로 리턴 (이후 호출부에서 req.image_data 유효성 검사로 필터링됨)
            req.image_data = nullptr;
        }

        return req;
    }

    InferClassID EngineClient::GetClassID( std::string_view class_name )
    {
        std::string upper_name = MGEN::ToUpperCase( std::string{ class_name } );

        auto it = this->target_class_ids.find( upper_name );
        if( it != this->target_class_ids.end() )
        {
            return it->second;
        }

        return INFER_CLASS_ID_NOT_SET;
    }

} // namespace MGEN