/**
 * @file EngineStreamTypes.h
 * @brief 추론 스트림의 상태, 해상도 키 및 메타데이터 정의를 담당합니다.
 */

#pragma once

#include "InferObject.h"   // InferObject 타입을 사용하기 위해 포함
#include "MgenTypes.h"     // UnitID, CameraColorMode 타입을 사용하기 위해 포함
#include "EngineProfile.h" // ModelMajorType, ModelMinorType 타입을 사용하기 위해 포함
#include "SafeQueue.h"     // sptrSafeQueue (EngineLinker 시그니처)

#include <vector>     // std::vector 컨테이너 사용
#include <memory>     // std::shared_ptr 사용
#include <functional> // std::hash 사용
#include <cstdint>    // uint64_t 등 정수 타입 사용
#include <tuple>      // std::tie를 이용한 비교 연산 구현을 위해 포함

namespace MGEN
{
    /**
     * @brief 엔진 추론 장치 식별자 타입 정의입니다.
     */
    using InferDeviceID = int;

    constexpr InferDeviceID INFER_DEVICE_ID_NOT_SET = -1;

    /**
     * @struct ResolutionKey
     * @brief 해상도 기반의 컨텍스트 관리를 위한 키 구조체입니다.
     */
    struct ResolutionKey
    {
        int w = 0; // 이미지 가로폭
        int h = 0; // 이미지 세로폭

        /**
         * @brief std::map 등의 정렬 컨테이너에서 키로 사용하기 위한 비교 연산자입니다.
         */
        bool operator<( const ResolutionKey& other ) const {
            return std::tie( w, h ) < std::tie( other.w, other.h );
        }

        bool operator==( const ResolutionKey& other ) const {
            return ( w == other.w ) && ( h == other.h );
        }
    };

    /**
     * @struct EngineActiveContext
     * @brief 현재 엔진의 활성화 상태 정보 (단일 엔진 RGB 환경에서 활성 조건 콜백에 전달).
     */
    struct EngineActiveContext
    {
        CameraColorMode color_mode = CameraColorMode::RGB; // 현재 카메라 색상 모드
    };

    // BASE ===================================================================
    constexpr Type::UnitID ENGINE_SUBSCRIBE_BASE_IDENTIFIER_JUMP_OFFSET = 10'000;

    // MAJOR ==================================================================
    constexpr Type::UnitID MAJOR_OFFSET_DIGITS  = 10'000;
    constexpr Type::UnitID MAJOR_DETECTION      = 0;

    // MAGIC ==================================================================
    constexpr Type::UnitID MAGIC_OFFSET_DIGITS  = 1;
    // MAGIC FLAGS (DETECTOR 단일 분기 — Detection 매직 타입 1개만 사용)
    constexpr Type::UnitID ENGINE_MAGIC_TYPE_DETECTION = 0;

    // JUMP OFFSETS ===========================================================
    // MAJOR OFFSET
    constexpr Type::UnitID ENGINE_SUBSCRIBE_MAJOR_IDENTIFIER_JUMP_OFFSET     = ENGINE_SUBSCRIBE_BASE_IDENTIFIER_JUMP_OFFSET  * MAJOR_OFFSET_DIGITS;
    constexpr Type::UnitID DETECTION_ENGINE_IDENTIFIER_JUMP_OFFSET           = ENGINE_SUBSCRIBE_MAJOR_IDENTIFIER_JUMP_OFFSET * MAJOR_DETECTION;

    // MAGIC OFFSET
    constexpr Type::UnitID ENGINE_SUBSCRIBE_MAGIC_IDENTIFIER_JUMP_OFFSET     = ENGINE_SUBSCRIBE_BASE_IDENTIFIER_JUMP_OFFSET  * MAGIC_OFFSET_DIGITS;
    constexpr Type::UnitID DETECTION_MAGIC_IDENTIFIER_JUMP_OFFSET            = ENGINE_SUBSCRIBE_MAGIC_IDENTIFIER_JUMP_OFFSET * ENGINE_MAGIC_TYPE_DETECTION;

    /**
     * @brief 엔진 식별자로부터 모델 대분류(Major Type)를 반환합니다.
     */
    inline ModelMajorType GetModelMajorTypeFromEngineIdentifier( const int identifier )
    {
        if( ( identifier / ENGINE_SUBSCRIBE_MAJOR_IDENTIFIER_JUMP_OFFSET ) == MAJOR_DETECTION )
        {
            return ModelMajorType::Detection;
        }

        return ModelMajorType::None;
    }

    Type::UnitID GetPureIDFromInferenceRequesterID( const Type::UnitID req_id );
    Type::UnitID GetMagicFlagFromInferenceRequesterID( const Type::UnitID req_id );
    Type::UnitID GetInferenceRequesterID( const Type::UnitID base_id, const ModelMajorType major_type, const ModelMinorType minor_type = ModelMinorType::None );
    Type::UnitID GetInferenceRequesterID( const Type::UnitID base_id, const ModelMajorType major_type, const ModelMinorType minor_type, const Type::UnitID magic_type );

    using EngineHandleUUID = std::pair<InferEngineID, InferDeviceID>;

    inline InferEngineID GetInferEngineID( const EngineHandleUUID& handle ) { return handle.first;  }
    inline InferDeviceID GetInferDeviceID( const EngineHandleUUID& handle ) { return handle.second; }

    /**
     * @struct EngineStreamMetaData
     * @brief 추론 요청 및 응답에 대한 매칭용 고유 식별자와 픽셀 정보를 포함합니다.
     */
    struct EngineStreamMetaData
    {
        Type::UnitID  requester_unit_id     = UNIT_ID_NOT_SET;         // 요청자 식별자
        InferEngineID requestee_engine_uuid = INFER_ENGINE_ID_NOT_SET; // 대상 엔진 식별자
        size_t        request_image_pixel_w = 0;                       // 요청 이미지 가로폭
        size_t        request_image_pixel_h = 0;                       // 요청 이미지 세로폭
        uint64_t      correlation_id        = 0;                       // 요청-응답 매칭용 시퀀스 번호
    };

    bool operator==( const EngineStreamMetaData& lhs, const EngineStreamMetaData& rhs );

    /**
     * @struct InputLayerWrapper
     * @brief 추론 엔진으로 전달될 이미지 데이터와 요구사항 명세입니다.
     */
    struct InputLayerWrapper
    {
        EngineStreamMetaData                        meta_data    { };         // 메타데이터
        InferRequestRequirements                    requirements { };         // 엔진 요구사항
        std::shared_ptr<std::vector<unsigned char>> image_data   { nullptr }; // 공유 이미지 데이터

        static InputLayerWrapper Build( const EngineStreamMetaData& meta_data, InferRequestRequirements requirements, std::shared_ptr<std::vector<unsigned char>> image_data )
        {
            return InputLayerWrapper{ meta_data, requirements, std::move( image_data ) };
        }
    };

    /**
     * @struct OutputLayerWrapper
     * @brief 엔진 추론 결과 객체들의 리스트와 실행 정보를 포함합니다.
     */
    struct OutputLayerWrapper
    {
        EngineStreamMetaData     meta_data          { };       // 메타데이터
        EngineHandleUUID         engine_handle_uuid { };       // 실제 처리를 담당한 엔진/장치 정보
        std::vector<InferObject> infer_objects      { };       // 탐색된 객체 리스트

        static OutputLayerWrapper Build( const EngineStreamMetaData& meta_data, const EngineHandleUUID& uuid, std::vector<InferObject>&& object )
        {
            return OutputLayerWrapper{ meta_data, uuid, std::move( object ) };
        }
    };

    bool operator==( const OutputLayerWrapper& lhs, const OutputLayerWrapper& rhs );

    /**
     * @brief 엔진 핸들러를 LoadBalancer 에 연결하는 함수 시그니처.
     * 단일 엔진 환경에서 핸들러가 자신의 입력 큐를 등록하고 응답 큐를 받아온다.
     */
    using EngineLinker
        = std::function<sptrSafeQueue<OutputLayerWrapper>(const EngineHandleUUID&, sptrSafeQueue<InputLayerWrapper>)>;

} // namespace MGEN

namespace std
{
    /**
     * @brief STL 컨테이너에서 EngineHandleUUID를 키로 사용하기 위한 해시 특수화입니다.
     */
    template <>
    struct hash<MGEN::EngineHandleUUID>
    {
        std::size_t operator()( const MGEN::EngineHandleUUID& handle ) const
        {
            std::size_t seed = 0;
            seed ^= std::hash<MGEN::InferEngineID>()( handle.first )  + MGEN::HASH_MAGIC_NUMB + ( seed << 6 ) + ( seed >> 2 );
            seed ^= std::hash<MGEN::InferDeviceID>()( handle.second ) + MGEN::HASH_MAGIC_NUMB + ( seed << 6 ) + ( seed >> 2 );
            return seed;
        }
    };

    /**
     * @brief EngineStreamMetaData에 대한 해시 특수화입니다.
     */
    template <>
    struct hash<MGEN::EngineStreamMetaData>
    {
        std::size_t operator()( const MGEN::EngineStreamMetaData& meta ) const
        {
            std::size_t h1   = std::hash<MGEN::Type::UnitID> ()( meta.requester_unit_id );
            std::size_t h2   = std::hash<MGEN::InferEngineID>()( meta.requestee_engine_uuid );
            std::size_t h3   = std::hash<uint64_t>()( meta.correlation_id );
            std::size_t seed = h1;

            seed ^= h2 + MGEN::HASH_MAGIC_NUMB + ( seed << 6 ) + ( seed >> 2 );
            seed ^= h3 + MGEN::HASH_MAGIC_NUMB + ( seed << 6 ) + ( seed >> 2 );
            return seed;
        }
    };

    /**
     * @brief OutputLayerWrapper에 대한 해시 특수화입니다.
     */
    template <>
    struct hash<MGEN::OutputLayerWrapper>
    {
        std::size_t operator()( const MGEN::OutputLayerWrapper& key ) const
        {
            std::size_t h1 = std::hash<MGEN::EngineStreamMetaData>()( key.meta_data );
            std::size_t h2 = 0;

            for( const auto& obj : key.infer_objects )
            {
                h2 ^= std::hash<MGEN::InferObject>()( obj ) + MGEN::HASH_MAGIC_NUMB + ( h2 << 6 ) + ( h2 >> 2 );
            }
            return h1 ^ ( h2 << 1 );
        }
    };
} // namespace std