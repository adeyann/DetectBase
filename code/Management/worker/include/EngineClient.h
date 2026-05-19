/**
 * @file EngineClient.h
 * @brief 추론 엔진과의 통신 및 메타데이터 관리를 담당하는 클라이언트 구조체를 정의합니다.
 */

#pragma once

#include "MgenTypes.h"            // UnitID 등 기초 타입 참조를 위해 포함
#include "EngineStreamTypes.h"    // InputLayerWrapper, EngineHandleUUID 등 참조를 위해 포함
#include "EngineLoadBalancer.h"   // 엔진 할당 및 상태 확인을 위해 포함
#include "EngineProfile.h"        // 엔진 사양(해상도, 모델 타입 등) 확인을 위해 포함
#include "FramePreProcessor.h"    // FrameFormattingContext 참조를 위해 포함
#include "ClassChecker.h"         // 클래스 이름-ID 매핑을 위해 포함

#include <string>       // std::string 사용
#include <vector>       // std::vector 사용
#include <map>          // 클래스 필터링 맵 사용
#include <memory>       // std::shared_ptr, std::unique_ptr 사용
#include <functional>   // std::function 사용
#include <string_view>  // 효율적인 문자열 참조를 위해 포함

namespace MGEN
{
    /**
     * @struct EngineClient
     * @brief 개별 추론 엔진에 대한 연결 상태와 요청 생성을 관리하는 객체입니다.
     */
    struct EngineClient
    {
        // Metadata -----------------------------------------------------------
        std::string magic_name; // 엔진의 고유 명칭 (예: "DetectionEngine")

        MGEN::Type::UnitID  subscribe_id = UNIT_ID_NOT_SET;         // 엔진 구독을 위한 고유 ID
        MGEN::InferEngineID engine_id    = INFER_ENGINE_ID_NOT_SET; // 할당된 엔진 인스턴스 ID

        // Model Profile ------------------------------------------------------
        MGEN::ModelMajorType    task_type     = MGEN::ModelMajorType::Detection; // 모델의 주요 작업 타입
        MGEN::ModelOptimizeType optimize_type = MGEN::ModelOptimizeType::None;   // 모델 최적화 방식

        int input_width  = 0; // 엔진 요구 입력 가로 해상도
        int input_height = 0; // 엔진 요구 입력 세로 해상도

        // Request Templates --------------------------------------------------
        /**
         * @brief 사전 빌드된 요청 메타데이터 래퍼입니다.
         * 실제 추론 시 이미지 데이터만 교체하여 재사용합니다.
         */
        std::shared_ptr<InputLayerWrapper> request_meta;

        // Condition & Filtering ----------------------------------------------
        /**
         * @brief 엔진의 활성화 여부를 판단하는 사용자 정의 조건 함수입니다.
         */
        std::function<bool( const EngineActiveContext& )> is_active_condition;

        /**
         * @brief 해당 엔진에서 감지하고자 하는 타겟 클래스 이름과 ID의 매핑 정보입니다.
         */
        std::map<std::string, MGEN::InferClassID> target_class_ids;

        // Interface Methods --------------------------------------------------

        /**
         * @brief 엔진 정보를 기반으로 클라이언트를 초기화하고 메타데이터를 구축합니다.
         * @param my_unit_id         요청 서비스의 UnitID
         * @param lb                 EngineLoadBalancer 인스턴스 포인터
         * @param target_magic_name  연결하고자 하는 엔진의 매직 네임
         * @param magic_code         ID 생성을 위한 고유 매직 코드
         * @param target_class_names 필터링하고자 하는 클래스 리스트
         * @param condition          엔진 활성화 여부를 결정할 콜백 함수
         * @return true 초기화 성공, false 엔진 미발견 혹은 프로파일 오류
         */
        bool Init(
            const MGEN::Type::UnitID                          my_unit_id,
            const std::shared_ptr<EngineLoadBalancer>&        lb,
            std::string_view                                  target_magic_name,
            const MGEN::Type::UnitID                          magic_code,
            const std::vector<std::string>&                   target_class_names,
            std::function<bool( const EngineActiveContext& )> condition
        );

        /**
         * @brief 전처리 컨텍스트(FFmpeg 기반)를 이용하여 추론 요청 객체를 생성합니다.
         * @param ctx            전처리가 완료된(혹은 예정인) 컨텍스트 포인터
         * @param correlation_id 요청-응답 매칭을 위한 고유 ID
         * @return InputLayerWrapper 완성된 추론 요청 객체
         */
        InputLayerWrapper BuildRequest( FrameFormattingContext* ctx, uint64_t correlation_id );

        /**
         * @brief 클래스 이름을 기반으로 사전 매핑된 클래스 ID를 반환합니다.
         * @param class_name 조회할 클래스 명칭 (대소문자 무관 처리 권장)
         * @return InferClassID 매핑된 ID, 존재하지 않을 경우 INFER_CLASS_ID_NOT_SET
         */
        InferClassID GetClassID( std::string_view class_name );
    };

} // namespace MGEN
