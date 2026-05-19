/**
 * @file FramePreProcessor.h
 * @brief InferObject.h에 정의된 스타일 타입을 재사용하여 영상 전처리(Resize, Padding, Blob화) 기능을 정의합니다.
 */

#pragma once

// OpenCV / FFmpeg 헤더 포함
#include <opencv2/opencv.hpp> // cv::Mat 및 이미지 처리 함수 사용을 위해 포함
extern "C" {
#include <libavutil/imgutils.h> // av_image_fill_linesizes, av_image_get_buffer_size 정의
#include <libavutil/avutil.h>
#include <libswscale/swscale.h> // SwsContext 및 이미지 변환을 위해 포함
#include <libavutil/frame.h>    // AVFrame 구조체 참조
#include <libavutil/pixfmt.h>   // AVPixelFormat 정의 참조
}

#include "EngineStreamTypes.h"  // 기초 스트림 타입 참조
#include "InferObject.h"        // ImageExpressStyle, BBoxCoordinateFormat 등 재사용을 위해 포함

#include <vector>  // std::vector 컨테이너 사용
#include <memory>  // std::shared_ptr 사용
#include <cmath>   // std::round 등 산술 함수 사용
#include <deque>   // 버퍼 풀을 위한 컨테이너
#include <mutex>

namespace MGEN
{
    /**
     * @brief SwsContext 자원 해제를 위한 커스텀 삭제자 함수입니다.
     */
    void free_sws_context( SwsContext* ctx );

    /**
     * @brief 재사용 가능한 원시 메모리 버퍼 타입 정의
     */
    using RawBuffer = std::vector<unsigned char>;

    struct FrameContextBufferPoolContainer
    {
        std::mutex mtx;
        std::deque<std::unique_ptr<RawBuffer>> pool;
        size_t max_size;

        explicit FrameContextBufferPoolContainer( size_t sz )
            : max_size(sz)
        {}
    };

    /**
     * @struct FrameFormattingContext
     * @brief 개별 엔진의 타겟 해상도별 전처리 상태와 버퍼를 관리하는 구조체입니다.
     * @details InferObject.h의 ImageExpressStyle을 사용하여 전처리 전후의 스타일 정보를 저장합니다.
     */
    struct FrameFormattingContext
    {
        bool is_initialized    = false;   // 초기화 완료 여부
        bool need_resize       = true;    // 리사이즈 필요 여부

        int  src_w             = 0;       // 원본 가로폭
        int  src_h             = 0;       // 원본 세로폭
        int  target_w          = 0;       // 타겟 가로폭
        int  target_h          = 0;       // 타겟 세로폭

        int  scale_w           = 0;       // 비율 유지 시 리사이즈될 가로폭
        int  scale_h           = 0;       // 비율 유지 시 리사이즈될 세로폭

        int  top               = 0;       // 상단 패딩 크기
        int  bottom            = 0;       // 하단 패딩 크기
        int  left              = 0;       // 좌측 패딩 크기
        int  right             = 0;       // 우측 패딩 크기

        std::shared_ptr<SwsContext> sws_scale_ctx = nullptr; // FFmpeg 스케일링 컨텍스트

        cv::Mat save_snapshot_mat; // 추론과 별개로 이미지 저장을 위해 캡처된 독립적인 이미지 데이터

        ImageExpressStyle inference_style;  // 추론 시점의 이미지 스타일 정보
        ImageExpressStyle original_style;   // 원본 시점의 이미지 스타일 정보

        std::shared_ptr<FrameContextBufferPoolContainer> pool_container = nullptr;

        /**
         * @brief 비동기 추론 안전성을 위해 매 프레임 새로운 메모리로 할당되는 블롭 데이터입니다.
         */
        std::shared_ptr<RawBuffer> shared_input_blob = nullptr;

        /**
         * @brief 해상도 및 포맷 변경에 따른 전처리 파라미터를 업데이트합니다.
         * @param new_src_w    새로운 원본 가로폭
         * @param new_src_h    새로운 원본 세로폭
         * @param new_target_w 새로운 타겟 가로폭
         * @param new_target_h 새로운 타겟 세로폭
         * @param format       AVPixelFormat 정수값
         * @return true 성공, false 실패
         */
        bool Update( int new_src_w, int new_src_h, int new_target_w, int new_target_h, int format );

        /**
         * @brief 영상 변환 및 데이터를 준비합니다.
         * @param frame 원본 AVFrame
         * @param capture_save_image 저장용 Mat 스냅샷을 생성할지 여부 (Default: false)
         * @param generate_inference_blob 추론용 공유 블롭을 생성할지 여부 (Default: true)
         */
        bool Convert( const std::shared_ptr<AVFrame>& frame, bool capture_save_image = false, bool generate_inference_blob = true );

        /**
         * @brief 추론에 즉시 투입 가능한 블롭 데이터가 준비되었는지 확인합니다.
         * @return true 준비 완료 (BuildRequest 가능), false 준비되지 않음
         */
        bool IsReadyForInference( void ) const;

        /**
         * @brief 해상도 변경 시 안전하게 풀을 비웁니다.
         */
        void ClearPool( void );

        bool InitPool( const size_t max_sz );

        bool IsResolutionChanged( int new_src_w, int new_src_h, int new_target_w, int new_target_h ) const;
    };

} // namespace MGEN