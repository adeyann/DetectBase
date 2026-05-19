/**
 * @file SwsContextManager.h
 * @brief FFmpeg SwsContext의 생성 및 해상도별 캐싱 관리를 담당합니다.
 */

#pragma once

// FFmpeg 관련 헤더 (C 라이브러리이므로 extern "C" 처리)
extern "C" {
#include <libswscale/swscale.h> // SwsContext 관리 및 변환 함수 사용
#include <libavutil/frame.h>    // AVFrame 구조체 참조
}

#include "EngineStreamTypes.h"  // ResolutionKey 및 기초 타입 참조
#include "FramePreProcessor.h"  // FrameFormattingContext 타입 참조

#include <map>    // 해상도별 컨텍스트 매핑을 위한 std::map 사용
#include <memory> // std::shared_ptr 사용

namespace MGEN
{
    /**
     * @class SwsContextManager
     * @brief 입력 프레임의 원본 해상도 변화를 감지하고 타겟 해상도별 전처리 컨텍스트를 관리합니다.
     */
    class SwsContextManager
    {
    public:
        /**
         * @brief 타겟 해상도에 해당하는 전처리 컨텍스트를 반환합니다.
         * @param target_w 타겟 가로폭
         * @param target_h 타겟 세로폭
         * @param frame    입력 원본 프레임 (원본 해상도 체크용)
         * @return FrameFormattingContext* 생성되거나 캐싱된 컨텍스트 포인터
         */
        FrameFormattingContext* GetContext( int target_w, int target_h, const std::shared_ptr<AVFrame>& frame );

        /**
         * @brief 관리 중인 모든 컨텍스트 리소스를 해제하고 초기화합니다.
         */
        void ClearAll( void );

    private:
        /**
         * @brief 해상도 키를 기반으로 개별 전처리 컨텍스트를 저장하는 맵입니다.
         */
        std::map<ResolutionKey, FrameFormattingContext> contexts;

        /**
         * @brief 현재 관리 중인 컨텍스트들의 기준 원본 해상도 정보입니다.
         */
        int current_src_w = 0;
        int current_src_h = 0;
    };

} // namespace MGEN