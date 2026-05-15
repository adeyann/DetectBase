/**
 * @file EngineStreamTypes.cpp
 * @brief EngineStreamTypes.h에 정의된 구조체 연산자 및 식별자 계산 로직을 구현합니다.
 */

#include "EngineStreamTypes.h" // 관련 타입 정의 참조를 위해 포함
#include <tuple>               // std::ignore

namespace MGEN
{
    /**
     * @brief EngineStreamMetaData 구조체의 동등 비교 연산자입니다.
     */
    bool operator==( const EngineStreamMetaData& lhs, const EngineStreamMetaData& rhs )
    {
        return ( lhs.requester_unit_id     == rhs.requester_unit_id )
            && ( lhs.requestee_engine_uuid == rhs.requestee_engine_uuid )
            && ( lhs.correlation_id        == rhs.correlation_id ); // 매칭 ID까지 엄격히 검사
    }

    /**
     * @brief OutputLayerWrapper 구조체의 동등 비교 연산자입니다.
     */
    bool operator==( const OutputLayerWrapper& lhs, const OutputLayerWrapper& rhs )
    {
        return ( lhs.meta_data          == rhs.meta_data )
            && ( lhs.engine_handle_uuid == rhs.engine_handle_uuid )
            && ( lhs.infer_objects      == rhs.infer_objects );
    }

    /**
     * @brief 기본 정보(Base/Major/Minor)를 조합하여 추론 요청자 식별자를 생성합니다.
     */
    Type::UnitID GetInferenceRequesterID(
        const Type::UnitID   base_id,
        const ModelMajorType major,
        const ModelMinorType minor
    )
    {
        UNUSED( minor );

        switch( major )
        {
            case ModelMajorType::Detection:
                return DETECTION_ENGINE_IDENTIFIER_JUMP_OFFSET + base_id;

            default:
                return base_id;
        }
    }

    /**
     * @brief Magic flag를 포함하여 상세화된 추론 요청자 식별자를 생성합니다.
     */
    Type::UnitID GetInferenceRequesterID(
        const Type::UnitID   base_id,
        const ModelMajorType major,
        const ModelMinorType minor,
        const Type::UnitID   magic
    )
    {
        Type::UnitID ret = base_id;

        /**
         * @note 현재 로직에서는 minor 타입을 사용하지 않으므로 경고 방지를 위해 UNUSED 처리합니다.
         */
        UNUSED( minor );

        switch( major )
        {
            case ModelMajorType::Detection:
            {
                ret += DETECTION_ENGINE_IDENTIFIER_JUMP_OFFSET;

                switch( magic )
                {
                    case ENGINE_MAGIC_TYPE_DETECTION: ret += DETECTION_MAGIC_IDENTIFIER_JUMP_OFFSET; break;
                    default: break;
                }
            }
            break;

            default: break;
        }

        return ret;
    }

    /**
     * @brief Jump Offset을 제거하고 순수하게 할당된 원본 ID만 추출합니다.
     */
    Type::UnitID GetPureIDFromInferenceRequesterID( const Type::UnitID req_id )
    {
        /**
         * @details Major Offset으로 나머지 연산 후, 다시 Magic Offset으로 나머지 연산을 수행하여 원본 ID를 도출합니다.
         */
        return ( req_id % ENGINE_SUBSCRIBE_MAJOR_IDENTIFIER_JUMP_OFFSET ) % ENGINE_SUBSCRIBE_MAGIC_IDENTIFIER_JUMP_OFFSET;
    }

    /**
     * @brief 식별자로부터 엔진의 특성을 나타내는 Magic Flag 값을 추출합니다.
     */
    Type::UnitID GetMagicFlagFromInferenceRequesterID( const Type::UnitID req_id )
    {
        return ( req_id % ENGINE_SUBSCRIBE_MAJOR_IDENTIFIER_JUMP_OFFSET ) / ENGINE_SUBSCRIBE_MAGIC_IDENTIFIER_JUMP_OFFSET;
    }

} // namespace MGEN