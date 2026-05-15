#include "rtsp_proxy_recorder.h"
#include "MgenLogger.h"

#include <cassert>
#include <cstring>
#include <chrono>
#include <vector>
#include <numeric>
#include <algorithm>
#include <optional>

/**
 * @brief Estimates the actual Frame Rate by analyzing unique PTS intervals
 * and filtering out sub-frame jitter (fragments < 10ms).
 */
static AVRational estimate_framerate( const std::deque<AVPacket*>& buffer )
{
    if( buffer.size() < 20 )
        return { 30, 1 };

    std::vector<int64_t> valid_intervals;
    const int64_t FRAGMENT_THRESHOLD_US = 10000; // 10ms jitter filter
    int64_t accumulated_delta = 0;

    for( size_t i = 1; i < buffer.size(); ++i ) {
        int64_t diff = buffer[i]->pts - buffer[i-1]->pts;

        if( diff < 0 )
            continue;

        accumulated_delta += diff;
        if( accumulated_delta >= FRAGMENT_THRESHOLD_US ) {
            valid_intervals.push_back( accumulated_delta );
            accumulated_delta = 0;
        }
    }

    if( valid_intervals.empty() )
        return { 30, 1 };

    int64_t sum           = std::accumulate( valid_intervals.begin(), valid_intervals.end(), 0LL );
    double  avg_interval  = static_cast<double>( sum ) / valid_intervals.size();
    double  fps           = 1000000.0 / avg_interval;

    return av_d2q( fps, 1000 );
}

CRtspProxyRecorder::CRtspProxyRecorder( const int proxy_id ) noexcept
    : _proxy_id       ( proxy_id )
    , _max_history_ns ( 20LL * 1000 * 1000 * 1000 ) // 20 sec
    , _v_codec_params ( nullptr )
{
    assert( _proxy_id >= 0 );

    _writer_thread.SetThreadFunctions(
        std::bind( &CRtspProxyRecorder::workerThreadRunner, this ),
        std::bind( &CRtspProxyRecorder::workerThreadCloser, this )
    );

    _writer_thread.Start();
}

CRtspProxyRecorder::~CRtspProxyRecorder()
{
    _writer_thread.Stop();

    if( _v_codec_params )
        avcodec_parameters_free( &_v_codec_params );

    std::lock_guard<std::mutex> lck( _history_mtx );
    while( !_history_buffer.empty() )
    {
        AVPacket* pkt = _history_buffer.front();

        if( pkt )
            av_packet_free( &pkt );

        _history_buffer.pop_front();
    }
}

void CRtspProxyRecorder::updateRealtimeFps( const int64_t current_pts )
{
    const int64_t FRAGMENT_THRESHOLD_US = 10000; // 10ms jitter filter
    auto now = std::chrono::steady_clock::now();

    if( _window_start_tp.time_since_epoch().count() == 0 )
        _window_start_tp = now;

    if( _last_pts_for_fps != AV_NOPTS_VALUE )
    {
        int64_t diff = current_pts - _last_pts_for_fps;
        if( diff >= 0 )
        {
            _interval_accumulator += diff;
            // 10ms 이상 쌓였을 때만 '하나의 유효 프레임'으로 간주하여 FPS 계산에 반영
            if( _interval_accumulator >= FRAGMENT_THRESHOLD_US )
            {
                _logical_frame_count++;
                _interval_accumulator = 0;
            }
        }
    }
    _last_pts_for_fps = current_pts;

    // 3sec 주기로 실시간 FPS 업데이트
    auto elpapsed = std::chrono::duration_cast<std::chrono::milliseconds>( now - _window_start_tp ).count();
    if( elpapsed >= 3000 )
    {
        if( _logical_frame_count > 0 )
        {
            double instant_fps = ( _logical_frame_count * 1000.0 ) / elpapsed;
            double prev_fps    = _realtime_fps.load( std::memory_order_relaxed );

            if( prev_fps <= 0 )
            {
                _realtime_fps.store( instant_fps, std::memory_order_relaxed );
            }
            else
            {
                // Exponential Moving Average (EMA)로 FPS 업데이트
                const double ema_alpha = 0.01;
                _realtime_fps.store( prev_fps + ema_alpha * ( instant_fps - prev_fps ), std::memory_order_relaxed );
            }

            _logical_frame_count = 0;
            _window_start_tp = now;
        }
    }
}

void CRtspProxyRecorder::pushVideoPacket( const AVPacket* pkt )
{
    if( !pkt || pkt->size <= 0 )
        return;

    updateParametersFromBitstream( pkt );

    AVPacket* cloned_pkt = av_packet_clone( pkt );
    if( !cloned_pkt )
        return;

    if( cloned_pkt->pts == AV_NOPTS_VALUE ){
        cloned_pkt->pts = av_gettime();
        cloned_pkt->dts = cloned_pkt->pts;
    }

    updateRealtimeFps( cloned_pkt->pts );

    {
        std::lock_guard<std::mutex> lck( _history_mtx );
        _history_buffer.push_back( cloned_pkt );
        manageHistoryBuffer();
    }

    _cv.notify_one();
}

void CRtspProxyRecorder::updateParametersFromBitstream( const AVPacket* pkt )
{
    if( !pkt || pkt->size <= 0 ) return;

    // 이미 충분한 크기(예: 10바이트 이상)의 extradata가 설정되었다면 중복 파싱을 방지합니다.
    if( _v_codec_params && _v_codec_params->extradata_size > 10 ) return;

    const uint8_t* data = pkt->data;
    const int       size = pkt->size;
    bool            new_info_collected = false;

    // 1. NAL 유닛 스캔 및 멤버 변수에 누적 저장
    for( int i = 0; i < size - 4; ++i )
    {
        int sc_len = 0;
        if( data[i] == 0 && data[i+1] == 0 && data[i+2] == 1 ) sc_len = 3;
        else if( data[i] == 0 && data[i+1] == 0 && data[i+2] == 0 && data[i+3] == 1 ) sc_len = 4;

        if( sc_len > 0 )
        {
            int nal_end = size;
            for( int j = i + sc_len; j < size - 4; ++j )
            {
                if( data[j] == 0 && data[j+1] == 0 && ( data[j+2] == 1 || ( data[j+2] == 0 && data[j+3] == 1 ) ) ) {
                    nal_end = j;
                    break;
                }
            }

            const uint8_t* nal_ptr = &data[i + sc_len];
            const int       nal_len = nal_end - ( i + sc_len );
            if (nal_len <= 0) continue;

            int nal_t = nal_ptr[0] & 0x1F;

            // SPS(7) 또는 PPS(8) 발견 시 멤버 변수에 업데이트 (누적)
            if( nal_t == 7 ) {
                _s_raw.assign( nal_ptr, nal_ptr + nal_len );
                _current_sps.valid = parse_sps( nal_ptr, nal_len, _current_sps );
                new_info_collected = true;
            }
            else if( nal_t == 8 ) {
                _p_raw.assign( nal_ptr, nal_ptr + nal_len );
                _current_pps.valid = parse_pps( nal_ptr, nal_len, _current_pps );
                new_info_collected = true;
            }
            i = nal_end - 1;
        }
    }

    // 2. 상태 로그 출력 (SPS나 PPS 중 하나라도 새로 발견되었을 때만)
    if( new_info_collected )
    {
        MLOG_INFO( "CAM<%d> Updated Codec Parameters from Bitstream: SPS(%s), PPS(%s)",
                   _proxy_id,
                   _current_sps.valid ? "Valid" : "Invalid",
                   _current_pps.valid ? "Valid" : "Invalid" );
    }

    // 3. SPS와 PPS가 모두 확보(Valid)되었다면 최종 extradata 조립
    if( _current_sps.valid && _current_pps.valid && !_s_raw.empty() && !_p_raw.empty() )
    {
        if( !_v_codec_params )
            _v_codec_params = avcodec_parameters_alloc();

        const uint8_t sc[] = { 0x00, 0x00, 0x00, 0x01 };
        size_t total = ( 4 + _s_raw.size() ) + ( 4 + _p_raw.size() );

        // 기존 extradata가 있다면 해제
        if( _v_codec_params->extradata )
            av_free( _v_codec_params->extradata );

        _v_codec_params->extradata = ( uint8_t* ) av_mallocz( total + AV_INPUT_BUFFER_PADDING_SIZE );
        uint8_t* dst = _v_codec_params->extradata;

        // [Annex B 형식 조립] 00 00 00 01 + SPS + 00 00 00 01 + PPS
        memcpy( dst, sc, 4 );
        dst += 4;
        memcpy( dst, _s_raw.data(), _s_raw.size() );
        dst += _s_raw.size();

        memcpy( dst, sc, 4 );
        dst += 4;
        memcpy( dst, _p_raw.data(), _p_raw.size() );
        dst += _p_raw.size();

        _v_codec_params->extradata_size = static_cast<int>( total );
        _v_codec_params->codec_id       = AV_CODEC_ID_H264;
        _v_codec_params->codec_type     = AVMEDIA_TYPE_VIDEO;
        _v_codec_params->width          = _current_sps.width;
        _v_codec_params->height         = _current_sps.height;

        MLOG_INFO( "CAM<%d> Extradata Composition Complete (%d bytes). Ready to record.",
                   _proxy_id, _v_codec_params->extradata_size );
    }
}

void CRtspProxyRecorder::manageHistoryBuffer( void )
{
    const int64_t threshold_us = _max_history_ns / 1000;
    while( _history_buffer.size() > 1 )
    {
        if( ( _history_buffer.back()->pts - _history_buffer.front()->pts ) > threshold_us )
        {
            AVPacket* old_pkt = _history_buffer.front();

            if( old_pkt )
                av_packet_free( &old_pkt );

            _history_buffer.pop_front();
        }
        else {
            break;
        }
    }
}

bool CRtspProxyRecorder::startRecording( const std::string& file_name, const int pre_roll_sec, const int post_roll_sec )
{
    if( !_v_codec_params || _v_codec_params->extradata_size == 0 )
        return false;

    auto task             = std::make_shared<RecordingTask>();
    task->fileName        = file_name;
    task->preRollUs       = static_cast<int64_t>( pre_roll_sec ) * 1000000;
    task->targetDuration  = static_cast<int64_t>( pre_roll_sec + post_roll_sec ) * 1000000;
    task->startSystemTime = av_gettime();

    {
        std::lock_guard<std::mutex> lck( _task_mtx );
        _active_tasks.push_back( task );
    }

    _cv.notify_one();

    return true;
}

void CRtspProxyRecorder::setVideoCodecParameters( const AVCodecParameters* params )
{
    if( !params )
        return;

    if( _v_codec_params )
        avcodec_parameters_free( &_v_codec_params );

    _v_codec_params = avcodec_parameters_alloc();

    if( _v_codec_params )
        avcodec_parameters_copy( _v_codec_params, params );
}

void CRtspProxyRecorder::workerThreadCloser( void )
{
    std::lock_guard<std::mutex> lck( _cv_mtx );
    _cv.notify_all();
}

void CRtspProxyRecorder::writeMergedPacketToDisk( std::shared_ptr<RecordingTask> task )
{
    if( !task || task->mergeBuffer.empty() || !task->fmtCtx )
        return;

    AVPacket* out = av_packet_alloc();
    if( !out )
        return;

    // 1. 병합된 데이터로 패킷 생성
    if( av_new_packet(out, task->mergeBuffer.size()) < 0 ){
        av_packet_free(&out);
        return;
    }
    memcpy( out->data, task->mergeBuffer.data(), task->mergeBuffer.size() );

    // 2. 시간축 동기화 (Normalize PTS)
    out->pts          = task->lastPtsInTask - task->firstPTS;
    out->dts          = out->pts;
    out->flags        = task->lastFlagsInTask;
    out->stream_index = 0;

    // 3. 프레임 지속 시간 계산 (잔상 방지 및 재생 속도 안정화)
    double current_fps = av_q2d( task->outStream->avg_frame_rate );
    out->duration = ( current_fps > 0 ) ? static_cast<int64_t>( 1000000 / current_fps ) : 33333;

    // 4. 파일에 기록
    int ret = av_interleaved_write_frame( task->fmtCtx, out );
    if( ret < 0 ){
        MLOG_ERROR("CAM[%d] Failed to write interleaved frame: %d", _proxy_id, ret);
    }

    // 5. 다음 병합을 위해 태스크 상태 리셋
    av_packet_free(&out);
    task->mergeBuffer.clear();
}

void CRtspProxyRecorder::workerThreadRunner( void )
{
    auto& is_running = _writer_thread.GetRunningFlag();

    while( is_running.load() )
    {
        std::unique_lock<std::mutex> lck_cv( _cv_mtx );
        _cv.wait_for( lck_cv, std::chrono::milliseconds( 100 ), [&is_running, this] {
            return !is_running.load() || !_active_tasks.empty();
        } );

        if( !is_running.load() )
            break;

        std::lock_guard<std::mutex> lck_task( _task_mtx );

        auto it = _active_tasks.begin();
        while( it != _active_tasks.end() )
        {
            auto task = *it;

            // --- STEP 1: Context & Header Initialization ---
            if( task->fmtCtx == nullptr )
            {
                if( avformat_alloc_output_context2( &task->fmtCtx, nullptr, "mp4", task->fileName.c_str() ) < 0 )
                {
                    it = _active_tasks.erase( it );
                    continue;
                }

                AVStream* st = avformat_new_stream( task->fmtCtx, nullptr );
                avcodec_parameters_copy( st->codecpar, _v_codec_params );

                AVRational est_fps = { 30, 1 };
                {
                    std::lock_guard<std::mutex> lck_h( _history_mtx );
                    est_fps = estimate_framerate( _history_buffer );
                }

                st->avg_frame_rate      = est_fps;
                st->r_frame_rate        = est_fps;
                st->time_base           = { 1, 1000000 };
                st->codecpar->codec_tag = 0;
                task->outStream         = st;

                if( avio_open( &task->fmtCtx->pb, task->fileName.c_str(), AVIO_FLAG_WRITE ) < 0 ){
                    avformat_free_context( task->fmtCtx );
                    it = _active_tasks.erase( it );
                    continue;
                }

                if( avformat_write_header( task->fmtCtx, nullptr ) < 0 ){
                    MLOG_ERROR( "Failed to write header: %s", task->fileName.c_str() );
                    avformat_free_context( task->fmtCtx );
                    it = _active_tasks.erase( it );
                    continue;
                }

                auto     opt_real_fps = this->getRealtimeFps();
                const double real_fps = opt_real_fps.has_value() ? opt_real_fps.value() : 0.0;

                MLOG_INFO( "Recorder Started | Cam: %d | FPS(use estimate): %.2f | FPS(use atomic): %.2f", _proxy_id, av_q2d( est_fps ), real_fps );
            }

            // --- STEP 2: Packet Processing & Merging ---
            {
                std::lock_guard<std::mutex> lck_hist( _history_mtx );
                if( _history_buffer.empty() ) { ++it; continue; }

                auto h_it = _history_buffer.begin();

                // Pre-roll (Historical) sync
                if( !task->isPreRollDone ) {
                    int64_t pre_limit = task->startSystemTime - task->preRollUs; // 10s pre-roll

                    for( ; h_it != _history_buffer.end(); ++h_it )
                    {
                        if( (*h_it)->pts >= pre_limit && ( (*h_it)->flags & AV_PKT_FLAG_KEY ) ) {
                            task->firstPTS = (*h_it)->pts;
                            task->isPreRollDone = true;
                            break;
                        }
                    }
                } else {
                    while( h_it != _history_buffer.end() && (*h_it)->pts <= task->lastWrittenPTS )
                        ++h_it;
                }

                // Main Merge Loop
                for( ; h_it != _history_buffer.end(); ++h_it )
                {
                    AVPacket* src_pkt = *h_it;
                    int64_t   norm_pts = src_pkt->pts - task->firstPTS;

                    if( norm_pts >= task->targetDuration ) task->isClosing = true;
                    if( task->isClosing && ( src_pkt->flags & AV_PKT_FLAG_KEY ) ) {
                        task->isFinished = true;
                        break;
                    }

                    // 정밀 병합 판단 로직 : Annex B 시작 코드 유무 + PTS 간격 분석
                    bool is_new_nal = false;
                    if( src_pkt->size >= 3 ){
                        if( src_pkt->data[0] == 0 && src_pkt->data[1] == 0 ){
                            if( src_pkt->data[2] == 1 ){
                                is_new_nal = true; // 00 00 01
                            }
                            else if( src_pkt->size >= 4 && src_pkt->data[2] == 0 && src_pkt->data[3] == 1 ){
                                is_new_nal = true; // 00 00 00 01
                            }
                        }
                    }

                    // 시작 코드가 없고, 이전 패킷과 PTS가 매우 가깝다면(5ms) 파편으로 간주
                    if( !is_new_nal && task->lastPtsInTask != -1 && std::abs( src_pkt->pts - task->lastPtsInTask ) < 5000 )
                    {
                        size_t old_sz = task->mergeBuffer.size();
                        task->mergeBuffer.resize( old_sz + src_pkt->size );
                        memcpy( task->mergeBuffer.data() + old_sz, src_pkt->data, src_pkt->size );
                        task->lastFlagsInTask |= src_pkt->flags;
                    }
                    else
                    {
                        // 새 프레임이 왔으므로 이전까지 모은 파편을 기록
                        writeMergedPacketToDisk(task);

                        // 새 병합 시작
                        task->mergeBuffer.assign( src_pkt->data, src_pkt->data + src_pkt->size );
                        task->lastPtsInTask   = src_pkt->pts;
                        task->lastFlagsInTask = src_pkt->flags;
                    }
                    task->lastWrittenPTS = src_pkt->pts;
                }
            }

            // --- STEP 3: Finalize Task ---
            if( task->isFinished )
            {
                writeMergedPacketToDisk(task); // 남은 버퍼 마지막 기록

                av_write_trailer( task->fmtCtx );

                avio_closep( &task->fmtCtx->pb );
                avformat_free_context( task->fmtCtx );

                MLOG_INFO( "CAM<%d> Save Completed : %s", _proxy_id, task->fileName.c_str() );

                it = _active_tasks.erase( it );
            }
            else { ++it; }
        }
    }
}

std::optional<double> CRtspProxyRecorder::getRealtimeFps( void ) const
{
    double fps = _realtime_fps.load( std::memory_order_relaxed );
    if( fps > 0 )
        return fps;
    else
        return std::nullopt;
}