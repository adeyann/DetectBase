/**
 * @file FramePreProcessor.cpp
 * @brief FrameFormattingContext의 영상 변환 및 메모리 관리 로직을 구현합니다.
 */

#include "FramePreProcessor.h" // 자신의 헤더 포함
#include "MgenLogger.h"        // 로그 출력을 위해 포함

namespace MGEN
{
    /**
     * @brief FFmpeg SwsContext 메모리 해제 헬퍼 함수입니다.
     */
    void free_sws_context( SwsContext* ctx )
    {
        if( ctx != nullptr )
        {
            sws_freeContext( ctx );
        }
    }

    void FrameFormattingContext::ClearPool( void )
    {
        if( this->pool_container )
        {
            std::lock_guard<std::mutex> lock( this->pool_container->mtx );
            this->pool_container->pool.clear();
        }
    }

    bool FrameFormattingContext::InitPool( const size_t max_sz )
    {
        this->pool_container = std::make_shared<FrameContextBufferPoolContainer>( max_sz );

        return (this->pool_container != nullptr);
    }

    bool FrameFormattingContext::IsResolutionChanged( int new_src_w, int new_src_h, int new_target_w, int new_target_h ) const
    {
        return ( src_w != new_src_w || src_h != new_src_h || target_w != new_target_w || target_h != new_target_h );
    }

    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    bool FrameFormattingContext::Update( int new_src_w, int new_src_h, int new_target_w, int new_target_h, int format )
    {
        // 해상도가 바뀌었는지 확인
        bool is_resolution_changed = this->IsResolutionChanged( new_src_w, new_src_h, new_target_w, new_target_h );
        if( is_initialized == true && is_resolution_changed == false ){
            return true;
        }

        if( pool_container == nullptr ){
            if( this->InitPool( 10 ) == false ){
                MLOG_ERROR( "FrameFormattingContext::Update() - Failed to initialize buffer pool" );
                return false;
            }
        }

        // 해상도가 바뀌었다면 기존 풀을 비움 (잘못된 크기의 버퍼 재사용 방지)
        if( is_resolution_changed == true ){
            this->ClearPool();
        }

        src_w    = new_src_w;
        src_h    = new_src_h;
        target_w = new_target_w;
        target_h = new_target_h;

        float  scale_factor = 1.0f;

        // 1. Letterbox (Resize & Padding) 계산
        if( src_w == target_w && src_h == target_h )
        {
            need_resize = false;
            scale_w     = src_w;
            scale_h     = src_h;
            left        = 0;
            right       = 0;
            top         = 0;
            bottom      = 0;
        }
        else
        {
            need_resize = true;

            float  w_ratio = static_cast<float>( target_w ) / static_cast<float>( src_w );
            float  h_ratio = static_cast<float>( target_h ) / static_cast<float>( src_h );

            scale_factor = std::min( w_ratio, h_ratio );

            scale_w = static_cast<int>( std::round( static_cast<float>( src_w ) * scale_factor ) );
            scale_h = static_cast<int>( std::round( static_cast<float>( src_h ) * scale_factor ) );

            float total_pad_w = static_cast<float>( target_w - scale_w );
            float total_pad_h = static_cast<float>( target_h - scale_h );

            total_pad_w /= 2.0f;
            total_pad_h /= 2.0f;

            // 정수 픽셀 배분을 위한 오프셋 조정
            left   = static_cast<int>( std::round( total_pad_w - 0.1f ) );
            right  = static_cast<int>( std::round( total_pad_w + 0.1f ) );
            top    = static_cast<int>( std::round( total_pad_h - 0.1f ) );
            bottom = static_cast<int>( std::round( total_pad_h + 0.1f ) );
        }

        // 3. SwsContext 설정 (YUV -> BGR24)
        AVPixelFormat src_fmt = static_cast<AVPixelFormat>( format );
        sws_scale_ctx = std::shared_ptr<SwsContext>(
            sws_getContext(
                src_w, src_h, src_fmt,
                scale_w, scale_h, AV_PIX_FMT_BGR24,
                SWS_AREA, nullptr, nullptr, nullptr
            ),
            free_sws_context
        );

        if( sws_scale_ctx == nullptr )
        {
            MLOG_ERROR( "FrameFormattingContext::Update() - Failed to create SwsContext" );
            return false;
        }

        // 4. Style 정보 업데이트 (InferObject.h의 구조체 활용)
        original_style  = ImageExpressStyle( src_w,    src_h,    BBoxCoordinateFormat::pixel_int,   BBoxReferenceType::ltx_type );
        inference_style = ImageExpressStyle( target_w, target_h, BBoxCoordinateFormat::ratio_float, BBoxReferenceType::ltx_type );

        inference_style.is_padded_or_scaled        = true;
        inference_style.scale_factor_from_original = scale_factor;
        inference_style.pad_x_offset               = left;
        inference_style.pad_y_offset               = top;
        inference_style.original_width             = src_w;
        inference_style.original_height            = src_h;

        is_initialized = true;
        return true;
    }

    /**
     * @brief 원본 데이터를 추론 블롭 및 저장용 스냅샷으로 변환합니다.
     * @param frame 원본 AVFrame 포인터
     * @param capture_save_image 저장용 Mat 스냅샷 생성 여부
     * @param generate_inference_blob 추론용 공유 블롭 생성 여부
     * @return true 변환 성공, false 실패
     */
    bool FrameFormattingContext::Convert
    (
        const std::shared_ptr<AVFrame>& frame,
        bool                            capture_save_image,
        bool                            generate_inference_blob
    )
    {
        if( is_initialized == false || sws_scale_ctx == nullptr || frame == nullptr ){
            return false;
        }

        if( pool_container == nullptr ){
            if( this->InitPool( 10 ) == false ){
                MLOG_ERROR( "FrameFormattingContext::Convert() - Failed to initialize buffer pool" );
                return false;
            }
        }

        const auto grey_color = cv::Scalar( 114, 114, 114 );

        // Stride 및 버퍼 크기 계산 (32바이트 정렬 일치)
        int line_size = ( target_w * 3 + 31 ) & ~31;

        // 전체 필요한 버퍼 크기 계산
        int required_size = av_image_get_buffer_size( AV_PIX_FMT_BGR24, target_w, target_h, 32 );

        // 추론용 블롭 생성이 필요한 경우
        if( generate_inference_blob == true )
        {
            std::unique_ptr<RawBuffer> target_vec = nullptr;
            {
                std::lock_guard<std::mutex> lock( this->pool_container->mtx );

                if( this->pool_container->pool.empty() == false )
                {
                    target_vec = std::move( this->pool_container->pool.front() );
                    this->pool_container->pool.pop_front();
                }
            }

            if( target_vec == nullptr ){
                target_vec = std::make_unique<RawBuffer>( required_size );
            }
            else if( target_vec->size() < static_cast<size_t>( required_size ) ){
                target_vec->resize( required_size );
            }

            cv::Mat full_mat( target_h, target_w, CV_8UC3, target_vec->data(), line_size );
            cv::Mat roi_mat = full_mat( cv::Rect( left, top, scale_w, scale_h ) );

            uint8_t* dst_data[AV_NUM_DATA_POINTERS] = { roi_mat.data, nullptr };
            int      dst_line[AV_NUM_DATA_POINTERS] = { static_cast<int>( roi_mat.step ), 0 };

            int processed_h = sws_scale( sws_scale_ctx.get(), frame->data, frame->linesize, 0, frame->height, dst_data, dst_line );
            if( processed_h <= 0 )
            {
                MLOG_ERROR( "sws_scale[1] failed with error code: %d", processed_h );
                return false;
            }

            if( need_resize == true )
            {
                if( top    > 0 ) full_mat( cv::Rect( 0, 0, target_w, top ) ).setTo( grey_color );
                if( bottom > 0 ) full_mat( cv::Rect( 0, target_h - bottom, target_w, bottom ) ).setTo( grey_color );
                if( left   > 0 ) full_mat( cv::Rect( 0, top, left, scale_h ) ).setTo( grey_color );
                if( right  > 0 ) full_mat( cv::Rect( target_w - right, top, right, scale_h ) ).setTo( grey_color );
            }

            if( capture_save_image == true ){
                full_mat.copyTo( save_snapshot_mat );
                if( save_snapshot_mat.empty() == true ){
                    return false;
                }
            }

            RawBuffer* raw_ptr = target_vec.release();

            // 캡처하여 람다 내부에서 안전하게 접근할 수 있도록 합니다.
            auto container = this->pool_container;

            shared_input_blob = std::shared_ptr<RawBuffer>(
                raw_ptr,
                [container]( RawBuffer* b ) {
                    std::lock_guard<std::mutex> lock( container->mtx );
                    if( container->pool.size() < container->max_size ) {
                        container->pool.emplace_back( std::unique_ptr<RawBuffer>( b ) );
                    }
                    else { delete b; }
                }
            );

            if( shared_input_blob == nullptr ){
                return false;
            }
        }
        // 저장용 이미지만 필요한 경우
        else if( capture_save_image == true )
        {
            shared_input_blob.reset();

            // 1. 버퍼 확보 및 ROI 설정
            save_snapshot_mat.create( target_h, target_w, CV_8UC3 );
            cv::Mat roi_mat = save_snapshot_mat( cv::Rect( left, top, scale_w, scale_h ) );

            // 2. ROI 영역에만 쓰기
            uint8_t* dst_data[AV_NUM_DATA_POINTERS] = { roi_mat.data, nullptr };
            int      dst_line[AV_NUM_DATA_POINTERS] = { static_cast<int>( roi_mat.step ), 0 };

            int processed_h = sws_scale( sws_scale_ctx.get(), frame->data, frame->linesize, 0, frame->height, dst_data, dst_line );
            if( processed_h <= 0 )
            {
                MLOG_ERROR( "sws_scale[2] failed with error code: %d", processed_h );
                return false;
            }

            // 3. 패딩 영역 처리 (저장용 이미지에서도 동일하게 적용)
            if( need_resize == true )
            {
                if( top    > 0 ) save_snapshot_mat( cv::Rect( 0, 0, target_w, top ) ).setTo( grey_color );
                if( bottom > 0 ) save_snapshot_mat( cv::Rect( 0, target_h - bottom, target_w, bottom ) ).setTo( grey_color );
                if( left   > 0 ) save_snapshot_mat( cv::Rect( 0, top, left, scale_h ) ).setTo( grey_color );
                if( right  > 0 ) save_snapshot_mat( cv::Rect( target_w - right, top, right, scale_h ) ).setTo( grey_color );
            }
        }

        return true;
    }

    /**
     * @brief 추론 준비 상태 체크
     * @details Convert()가 성공적으로 수행되어 shared_input_blob이 유효한지 검사합니다.
     */
    bool FrameFormattingContext::IsReadyForInference( void ) const
    {
        /**
         * 단순히 포인터 존재 여부와 실제 데이터가 차 있는지를 체크하여
         * BuildRequest가 안전하게 수행될 수 있음을 보장합니다.
         */
        return ( shared_input_blob != nullptr && shared_input_blob->empty() == false );
    }

} // namespace MGEN