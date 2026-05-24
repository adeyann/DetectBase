/**
 * @file GstRtspReceiver.cpp
 * @brief GstRtspReceiver — 단일 pipeline (avdec_h264 / I420 / PoC 검증 구조).
 *
 * 2-pipeline architecture / mppvideodec 모두 제거 — 사용자 명령 (2026-05-15).
 *   reason: 매 reconnect 시 leak 발생. mppvideodec 외 다른 변수 영향도 큼.
 *   PoC 단위 14/14 PASS 시점 구조로 롤백 후 재측정.
 */

#include "GstRtspReceiver.h"

#include "MgenLogger.h"
#include "MetricsRegistry.h"

#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <gst/video/video.h>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

#include <chrono>
#include <cstring>
#include <mutex>

namespace MGEN
{
    namespace
    {
        constexpr const char* METRIC_FRAMES_TOTAL       = "detectbase_gst_rtsp_frames_total";
        constexpr const char* METRIC_RECONNECT_TOTAL    = "detectbase_gst_rtsp_reconnect_total";
        constexpr const char* METRIC_ERRORS_TOTAL       = "detectbase_gst_rtsp_errors_total";
        // 진단 (debug/gst-rtsp-stale-trace 2026-05-20)
        constexpr const char* METRIC_BUS_MSG_TOTAL      = "detectbase_gst_rtsp_bus_message_total";
        constexpr const char* METRIC_RESET_ATTEMPT      = "detectbase_gst_rtsp_reset_attempt_total";
        constexpr const char* METRIC_LAST_FRAME_AGE_SEC = "detectbase_gst_rtsp_last_frame_age_sec";
        // 측정 (experiment/rtcp-timeout-measure 2026-05-21) — RTCP timeout 발생 빈도 측정용.
        //   현재 cause=RTCP 는 무시 (reconnect 안 함) — behavior 변경 없이 count 만 추가.
        constexpr const char* METRIC_RTCP_TIMEOUT_TOTAL = "detectbase_gst_rtsp_rtcp_timeout_total";
        // 측정 (2026-05-21) — pipeline 단계별 buffer flow per cam (stuck 위치 확정용)
        constexpr const char* METRIC_DEPAY_BUFFER_TOTAL = "detectbase_gst_rtsp_depay_buffer_total";  // depay src (pre-decode)
        constexpr const char* METRIC_DECODED_TOTAL      = "detectbase_gst_rtsp_decoded_total";       // appsink (decoded), per cam
        // 검증 (2026-05-21) — depay sink (rtspsrc → depay 진입) + jitterbuffer stats
        constexpr const char* METRIC_RTP_IN_TOTAL       = "detectbase_gst_rtsp_rtp_in_total";        // depay sink (rtspsrc 출력)
        constexpr const char* METRIC_JB_PUSHED          = "detectbase_gst_rtsp_jb_num_pushed";       // jitterbuffer num-pushed (gauge)
        constexpr const char* METRIC_JB_LOST            = "detectbase_gst_rtsp_jb_num_lost";         // jitterbuffer num-lost (gauge)
        constexpr const char* METRIC_JB_RTX_COUNT       = "detectbase_gst_rtsp_jb_rtx_count";        // jitterbuffer rtx-count (gauge)

        const std::map<std::string, std::string> NO_LABELS;

        std::once_flag g_recv_metrics_once;

        void RegisterMetricsOnce() noexcept
        {
            std::call_once( g_recv_metrics_once, []{
                auto& reg = MetricsRegistry::Instance();
                reg.RegisterCounter( METRIC_FRAMES_TOTAL,    "GStreamer RTSP frames received and decoded" );
                reg.RegisterCounter( METRIC_RECONNECT_TOTAL, "GStreamer RTSP reconnect attempts" );
                reg.RegisterCounter( METRIC_ERRORS_TOTAL,    "GStreamer RTSP bus error events" );
                reg.RegisterCounter( METRIC_BUS_MSG_TOTAL,   "GStreamer RTSP bus message count by cam_id and type (debug trace)" );
                reg.RegisterCounter( METRIC_RESET_ATTEMPT,   "GstRtspReceiver::ResetSourceOnly attempt count by cam_id and result (debug trace)" );
                reg.RegisterGauge  ( METRIC_LAST_FRAME_AGE_SEC, "Seconds since last frame received per cam_id (debug trace)" );
                reg.RegisterCounter( METRIC_RTCP_TIMEOUT_TOTAL, "GstRTSPSrcTimeout cause=RTCP occurrences per cam_id (measurement — behavior unchanged)" );
                reg.RegisterCounter( METRIC_DEPAY_BUFFER_TOTAL, "RTP/pre-decode buffers passing rtph264depay src pad per cam_id (stuck-stage trace)" );
                reg.RegisterCounter( METRIC_DECODED_TOTAL,      "Decoded frames reaching appsink per cam_id (stuck-stage trace)" );
                reg.RegisterCounter( METRIC_RTP_IN_TOTAL,       "Buffers entering rtph264depay sink pad per cam_id (rtspsrc output — stuck-stage trace)" );
                reg.RegisterGauge  ( METRIC_JB_PUSHED,          "rtpjitterbuffer num-pushed per cam_id" );
                reg.RegisterGauge  ( METRIC_JB_LOST,            "rtpjitterbuffer num-lost per cam_id" );
                reg.RegisterGauge  ( METRIC_JB_RTX_COUNT,       "rtpjitterbuffer rtx-count per cam_id" );
            } );
        }
    } // anonymous

    GstRtspReceiver::GstRtspReceiver( const Config& cfg, FrameCallback cb ) noexcept
        : cfg_( cfg )
        , cb_ ( std::move( cb ) )
    {
        RegisterMetricsOnce();
        MLOG_INFO( "GstRtspReceiver[%d] 생성 — url=%s latency=%dms (single-pipeline)",
            cfg_.cam_id, cfg_.url.c_str(), cfg_.latency_ms );
    }

    GstRtspReceiver::~GstRtspReceiver()
    {
        Stop();
        MLOG_INFO( "GstRtspReceiver 종료 — frames=%lu reconnect=%lu errors=%lu",
            (unsigned long) frame_count_.load(),
            (unsigned long) reconnect_count_.load(),
            (unsigned long) error_count_.load() );
    }

    uint32_t GstRtspReceiver::GetAppSinkQueuedBuffers() const noexcept
    {
        // GStreamer pipeline 내부 누적 측정 — appsink 의 current-level-buffers property.
        //   appsink queue 가 cam thread dequeue 못 따라가면 누적.
        //   property 명: "current-level-buffers" (GStreamer 1.18+).
        uint32_t total = 0;
        if( appsink_ ) {
            guint level = 0;
            g_object_get( const_cast<GstAppSink*>( appsink_ ), "current-level-buffers", &level, nullptr );
            total += static_cast<uint32_t>( level );
        }
        if( raw_appsink_ ) {
            guint level = 0;
            g_object_get( const_cast<GstAppSink*>( raw_appsink_ ), "current-level-buffers", &level, nullptr );
            total += static_cast<uint32_t>( level );
        }
        return total;
    }

    void GstRtspReceiver::SetRawPacketCallback( RawPacketCallback cb ) noexcept
    {
        raw_cb_ = std::move( cb );
    }

    bool GstRtspReceiver::BuildPipeline()
    {
        gchar* desc = nullptr;
        if( cfg_.enable_raw_passthrough ) {
            desc = g_strdup_printf(
                "rtspsrc name=src location=%s latency=%d "
                "    timeout=%d tcp-timeout=%d do-retransmission=%s "
                "    keepalive=true udp-buffer-size=2097152 "
                "    user-id=%s user-pw=%s "
                "  ! rtph264depay name=depay ! h264parse "
                "  ! tee name=tee_h264 "
                "tee_h264. ! queue max-size-buffers=4 leaky=downstream "
                "  ! avdec_h264 ! videoconvert ! video/x-raw,format=I420 "
                "  ! appsink name=sink emit-signals=true sync=false max-buffers=%d drop=true "
                "tee_h264. ! queue max-size-buffers=8 leaky=downstream "
                "  ! rtph264pay pt=96 config-interval=1 "
                "  ! appsink name=raw_sink emit-signals=true sync=false max-buffers=20 drop=true",
                cfg_.url.c_str(),
                cfg_.latency_ms,
                cfg_.timeout_us,
                cfg_.tcp_timeout_us,
                cfg_.do_retransmission ? "true" : "false",
                cfg_.user_id.c_str(),
                cfg_.user_pw.c_str(),
                cfg_.appsink_max_buffers );
        } else {
            desc = g_strdup_printf(
                "rtspsrc name=src location=%s latency=%d "
                "    timeout=%d tcp-timeout=%d do-retransmission=%s "
                "    keepalive=true udp-buffer-size=2097152 "
                "    user-id=%s user-pw=%s "
                "  ! rtph264depay name=depay ! h264parse "
                // 검증 (2026-05-21): decouple queue 추가. raw 변형엔 있으나 normal 에 누락됐던 것.
                //   leaky=downstream → decode 지연 시 upstream(udpsrc) backpressure 차단 (frame drop 으로 흡수).
                //   가설: 이 queue 부재가 cam stuck (udpsrc socket read 중단) 의 원인.
                "  ! queue name=decodeq max-size-buffers=4 leaky=downstream "
                "  ! avdec_h264 ! videoconvert ! video/x-raw,format=I420 "
                "  ! appsink name=sink emit-signals=true sync=false max-buffers=%d drop=true",
                cfg_.url.c_str(),
                cfg_.latency_ms,
                cfg_.timeout_us,
                cfg_.tcp_timeout_us,
                cfg_.do_retransmission ? "true" : "false",
                cfg_.user_id.c_str(),
                cfg_.user_pw.c_str(),
                cfg_.appsink_max_buffers );
        }

        // 측정 (2026-05-21): timeout 값이 실제 pipeline 에 적용되는지 검증용 desc log
        MLOG_INFO( "GstRtspReceiver[%d] pipeline desc: %s", cfg_.cam_id, desc );

        GError* err = nullptr;
        pipeline_ = gst_parse_launch( desc, &err );
        g_free( desc );

        if( !pipeline_ ) {
            MLOG_ERROR( "gst_parse_launch 실패: %s", err ? err->message : "(unknown)" );
            if( err ) g_error_free( err );
            return false;
        }

        appsink_ = GST_APP_SINK( gst_bin_get_by_name( GST_BIN( pipeline_ ), "sink" ) );
        if( !appsink_ ) {
            MLOG_ERROR( "appsink(name='sink') not found in pipeline" );
            gst_object_unref( pipeline_ );
            pipeline_ = nullptr;
            return false;
        }

        GstAppSinkCallbacks callbacks {};
        callbacks.new_sample = &GstRtspReceiver::OnNewSample;
        gst_app_sink_set_callbacks( appsink_, &callbacks, this, nullptr );

        if( cfg_.enable_raw_passthrough ) {
            raw_appsink_ = GST_APP_SINK( gst_bin_get_by_name( GST_BIN( pipeline_ ), "raw_sink" ) );
            if( !raw_appsink_ ) {
                MLOG_ERROR( "raw_appsink(name='raw_sink') not found in pipeline" );
                gst_object_unref( appsink_ );
                appsink_ = nullptr;
                gst_object_unref( pipeline_ );
                pipeline_ = nullptr;
                return false;
            }
            GstAppSinkCallbacks raw_cbs {};
            raw_cbs.new_sample = &GstRtspReceiver::OnNewRawSample;
            gst_app_sink_set_callbacks( raw_appsink_, &raw_cbs, this, nullptr );
        }

        GstBus* bus = gst_pipeline_get_bus( GST_PIPELINE( pipeline_ ) );
        bus_watch_id_ = gst_bus_add_watch( bus, &GstRtspReceiver::OnBusMessage, this );
        gst_object_unref( bus );

        // 측정 (2026-05-21): depay src pad 에 buffer probe 추가 — RTP/pre-decode flow per cam.
        //   probe 는 pipeline destroy 시 자동 제거됨. element ref 는 즉시 unref.
        GstElement* depay = gst_bin_get_by_name( GST_BIN( pipeline_ ), "depay" );
        if( depay ) {
            GstPad* src_pad = gst_element_get_static_pad( depay, "src" );
            if( src_pad ) {
                gst_pad_add_probe( src_pad, GST_PAD_PROBE_TYPE_BUFFER,
                    &GstRtspReceiver::OnDepayBufferProbe, this, nullptr );
                gst_object_unref( src_pad );
            }
            // 검증 (2026-05-22): depay SINK probe(rtp_in, ~1800/s/cam) 제거 — per-packet
            //   prometheus contention 이 loss burst 트리거인지 테스트. burst/stuck 사라지면 계측이 원인.
            //   (depay src probe 는 per-NAL ~90/s 로 유지, stage 가시성용)
            gst_object_unref( depay );
        }

        // 검증 (2026-05-21): rtspsrc 의 rtpbin manager → jitterbuffer 캡처 (stats 추적)
        jitterbuffer_ = nullptr;
        GstElement* src = gst_bin_get_by_name( GST_BIN( pipeline_ ), "src" );
        if( src ) {
            g_signal_connect( src, "new-manager", G_CALLBACK( &GstRtspReceiver::OnNewManager ), this );
            gst_object_unref( src );
        }

        // 검증 (2026-05-21): jitterbuffer stats timer 를 BuildPipeline 에서 추가 (매 EOS rebuild 마다 재등록).
        //   이전 버그: MainLoopThreadFunc 에서 1회만 추가 → 첫 reset 의 Teardown 이 제거 후 재등록 안 됨 → gauge frozen.
        jitter_timer_id_ = g_timeout_add( 2000, &GstRtspReceiver::OnJitterStatsTimer, this );

        return true;
    }

    void GstRtspReceiver::OnNewManager( GstElement* /*rtspsrc*/, GstElement* manager, void* user_data )
    {
        // rtpbin 의 new-jitterbuffer 신호 연결 → jitterbuffer element 캡처
        g_signal_connect( manager, "new-jitterbuffer",
            G_CALLBACK( &GstRtspReceiver::OnNewJitterbuffer ), user_data );
    }

    void GstRtspReceiver::OnNewJitterbuffer( GstElement* /*rtpbin*/, GstElement* jb, guint session, guint /*ssrc*/, void* user_data )
    {
        auto* self = static_cast<GstRtspReceiver*>( user_data );
        // 첫 session (video) 의 jitterbuffer 만 추적 (weak ref — pipeline 소유)
        if( !self->jitterbuffer_ ) {
            self->jitterbuffer_ = jb;
            MLOG_INFO( "GstRtspReceiver[%d] jitterbuffer 캡처 (session=%u)", self->cfg_.cam_id, session );
        }
    }

    gboolean GstRtspReceiver::OnJitterStatsTimer( void* user_data )
    {
        auto* self = static_cast<GstRtspReceiver*>( user_data );
        if( !self->running_.load() || !self->jitterbuffer_ ) return TRUE;

        GstStructure* stats = nullptr;
        g_object_get( self->jitterbuffer_, "stats", &stats, nullptr );
        if( stats ) {
            guint64 pushed = 0, lost = 0, rtx_count = 0, late = 0;
            gst_structure_get_uint64( stats, "num-pushed", &pushed );
            gst_structure_get_uint64( stats, "num-lost",   &lost );
            gst_structure_get_uint64( stats, "num-late",   &late );
            gst_structure_get_uint64( stats, "rtx-count",  &rtx_count );
            const std::map<std::string, std::string> labels { { "cam_id", std::to_string( self->cfg_.cam_id ) } };
            auto& reg = MetricsRegistry::Instance();
            reg.SetGauge( METRIC_JB_PUSHED,    labels, static_cast<double>( pushed ) );
            reg.SetGauge( METRIC_JB_LOST,      labels, static_cast<double>( lost ) );
            reg.SetGauge( METRIC_JB_RTX_COUNT, labels, static_cast<double>( rtx_count ) );
            gst_structure_free( stats );
        }
        return TRUE;  // 반복
    }

    GstPadProbeReturn GstRtspReceiver::OnDepayBufferProbe( GstPad* /*pad*/, GstPadProbeInfo* /*info*/, void* user_data )
    {
        auto* self = static_cast<GstRtspReceiver*>( user_data );
        MetricsRegistry::Instance().IncrementCounter(
            METRIC_DEPAY_BUFFER_TOTAL,
            { { "cam_id", std::to_string( self->cfg_.cam_id ) } }, 1.0 );
        return GST_PAD_PROBE_OK;
    }

    GstPadProbeReturn GstRtspReceiver::OnRtpInProbe( GstPad* /*pad*/, GstPadProbeInfo* /*info*/, void* user_data )
    {
        auto* self = static_cast<GstRtspReceiver*>( user_data );
        MetricsRegistry::Instance().IncrementCounter(
            METRIC_RTP_IN_TOTAL,
            { { "cam_id", std::to_string( self->cfg_.cam_id ) } }, 1.0 );
        return GST_PAD_PROBE_OK;
    }

    void GstRtspReceiver::TeardownPipeline()
    {
        if( pipeline_ ) {
            if( jitter_timer_id_ ) {
                g_source_remove( jitter_timer_id_ );
                jitter_timer_id_ = 0;
            }
            jitterbuffer_ = nullptr;  // weak ref — pipeline 이 곧 destroy
            if( bus_watch_id_ ) {
                g_source_remove( bus_watch_id_ );
                bus_watch_id_ = 0;
            }
            gst_element_set_state( pipeline_, GST_STATE_NULL );
            GstState st;
            gst_element_get_state( pipeline_, &st, nullptr, 5 * GST_SECOND );
            if( appsink_ ) {
                gst_object_unref( appsink_ );
                appsink_ = nullptr;
            }
            if( raw_appsink_ ) {
                gst_object_unref( raw_appsink_ );
                raw_appsink_ = nullptr;
            }
            gst_object_unref( pipeline_ );
            pipeline_ = nullptr;
        }
    }

    void GstRtspReceiver::MainLoopThreadFunc()
    {
        loop_ = g_main_loop_new( nullptr, FALSE );
        g_main_loop_run( loop_ );
        g_main_loop_unref( loop_ );
        loop_ = nullptr;
    }

    bool GstRtspReceiver::Start() noexcept
    {
        if( running_.exchange( true ) ) {
            MLOG_WARN( "GstRtspReceiver::Start 이미 실행 중 — 무시" );
            return false;
        }

        if( !BuildPipeline() ) {
            running_.store( false );
            return false;
        }

        GstStateChangeReturn ret = gst_element_set_state( pipeline_, GST_STATE_PLAYING );
        if( ret == GST_STATE_CHANGE_FAILURE ) {
            MLOG_ERROR( "gst_element_set_state(PLAYING) FAILURE" );
            TeardownPipeline();
            running_.store( false );
            return false;
        }

        loop_thread_ = std::thread( [this]{ MainLoopThreadFunc(); } );
        MLOG_INFO( "GstRtspReceiver::Start OK — url=%s", cfg_.url.c_str() );
        return true;
    }

    void GstRtspReceiver::Stop() noexcept
    {
        if( !running_.exchange( false ) ) return;

        if( loop_ ) {
            g_main_loop_quit( loop_ );
        }
        if( loop_thread_.joinable() ) {
            loop_thread_.join();
        }
        TeardownPipeline();
        MLOG_INFO( "GstRtspReceiver::Stop OK" );
    }

    bool GstRtspReceiver::ResetSourceOnly() noexcept
    {
        const auto t_start = std::chrono::steady_clock::now();
        auto& reg = MetricsRegistry::Instance();
        const std::string cam_id_str = std::to_string( cfg_.cam_id );

        if( !pipeline_ ) {
            MLOG_WARN( "ResetSourceOnly[%d] — pipeline_ is nullptr (skipped)", cfg_.cam_id );
            reg.IncrementCounter( METRIC_RESET_ATTEMPT, { { "cam_id", cam_id_str }, { "result", "no_pipeline" } }, 1.0 );
            return false;
        }

        reset_source_count_.fetch_add( 1, std::memory_order_relaxed );
        MLOG_INFO( "ResetSourceOnly[%d] 진입 — pipeline destroy/rebuild (single), reset_cnt=%lu",
            cfg_.cam_id, (unsigned long) reset_source_count_.load() );
        reg.IncrementCounter( METRIC_RESET_ATTEMPT, { { "cam_id", cam_id_str }, { "result", "enter" } }, 1.0 );

        TeardownPipeline();

        if( !BuildPipeline() ) {
            MLOG_ERROR( "ResetSourceOnly[%d] — BuildPipeline 실패", cfg_.cam_id );
            reg.IncrementCounter( METRIC_RESET_ATTEMPT, { { "cam_id", cam_id_str }, { "result", "build_fail" } }, 1.0 );
            return false;
        }
        GstStateChangeReturn r = gst_element_set_state( pipeline_, GST_STATE_PLAYING );
        if( r == GST_STATE_CHANGE_FAILURE ) {
            MLOG_ERROR( "ResetSourceOnly[%d] — PLAYING 실패", cfg_.cam_id );
            reg.IncrementCounter( METRIC_RESET_ATTEMPT, { { "cam_id", cam_id_str }, { "result", "playing_fail" } }, 1.0 );
            TeardownPipeline();
            return false;
        }
        const auto t_end = std::chrono::steady_clock::now();
        const int64_t dur_us = std::chrono::duration_cast<std::chrono::microseconds>( t_end - t_start ).count();
        MLOG_INFO( "ResetSourceOnly[%d] OK — duration=%ldus", cfg_.cam_id, (long) dur_us );
        reg.IncrementCounter( METRIC_RESET_ATTEMPT, { { "cam_id", cam_id_str }, { "result", "ok" } }, 1.0 );
        return true;
    }

    GstFlowReturn GstRtspReceiver::OnNewSample( GstAppSink* sink, void* user_data )
    {
        auto* self = static_cast<GstRtspReceiver*>( user_data );
        GstSample* sample = gst_app_sink_pull_sample( sink );
        if( !sample ) return GST_FLOW_OK;

        std::shared_ptr<AVFrame> frame = self->ConvertSampleToAVFrame( sample );
        gst_sample_unref( sample );

        if( !frame ) {
            return GST_FLOW_OK;
        }

        self->frame_count_.fetch_add( 1 );
        MetricsRegistry::Instance().IncrementCounter( METRIC_FRAMES_TOTAL, NO_LABELS, 1.0 );
        // 측정 (2026-05-21): per-cam decoded frame count (stuck-stage trace)
        MetricsRegistry::Instance().IncrementCounter(
            METRIC_DECODED_TOTAL, { { "cam_id", std::to_string( self->cfg_.cam_id ) } }, 1.0 );

        // 진단 (debug/gst-rtsp-stale-trace 2026-05-20) — last_frame timestamp 기록
        const int64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                  std::chrono::steady_clock::now().time_since_epoch() ).count();
        self->last_frame_ns_.store( now_ns, std::memory_order_relaxed );

        if( self->cb_ ) {
            try {
                self->cb_( frame );
            } catch( ... ) {
                MLOG_ERROR( "frame callback threw exception" );
            }
        }
        return GST_FLOW_OK;
    }

    GstFlowReturn GstRtspReceiver::OnNewRawSample( GstAppSink* sink, void* user_data )
    {
        auto* self = static_cast<GstRtspReceiver*>( user_data );
        GstSample* sample = gst_app_sink_pull_sample( sink );
        if( !sample ) return GST_FLOW_OK;

        GstBuffer* buf = gst_sample_get_buffer( sample );
        if( buf && self->raw_cb_ ) {
            GstMapInfo info;
            if( gst_buffer_map( buf, &info, GST_MAP_READ ) ) {
                try {
                    self->raw_cb_( info.data, info.size );
                } catch( ... ) {
                    MLOG_ERROR( "raw_cb threw exception" );
                }
                gst_buffer_unmap( buf, &info );
            }
        }
        gst_sample_unref( sample );
        return GST_FLOW_OK;
    }

    std::shared_ptr<AVFrame> GstRtspReceiver::ConvertSampleToAVFrame( GstSample* sample ) noexcept
    {
        GstBuffer* buf  = gst_sample_get_buffer( sample );
        GstCaps*   caps = gst_sample_get_caps( sample );
        if( !buf || !caps ) return nullptr;

        GstVideoInfo info;
        if( !gst_video_info_from_caps( &info, caps ) ) {
            MLOG_ERROR( "gst_video_info_from_caps 실패" );
            return nullptr;
        }
        if( GST_VIDEO_INFO_FORMAT( &info ) != GST_VIDEO_FORMAT_I420 ) {
            MLOG_ERROR( "예상 I420 아님 (format=%d)", (int) GST_VIDEO_INFO_FORMAT( &info ) );
            return nullptr;
        }

        AVFrame* frame = av_frame_alloc();
        if( !frame ) return nullptr;

        frame->width  = info.width;
        frame->height = info.height;
        frame->format = AV_PIX_FMT_YUV420P;
        if( av_frame_get_buffer( frame, 32 ) < 0 ) {
            av_frame_free( &frame );
            return nullptr;
        }

        GstMapInfo map;
        if( !gst_buffer_map( buf, &map, GST_MAP_READ ) ) {
            av_frame_free( &frame );
            return nullptr;
        }

        const size_t y_sz = (size_t) info.width * info.height;
        const size_t u_sz = (size_t) ( info.width / 2 ) * ( info.height / 2 );
        const size_t v_sz = u_sz;
        const uint8_t* src = map.data;

        // Q1 fix (심층 review 2026-05-15): 이전 코드는 buffer size 부족 시 memcpy 만
        // skip 하고 frame 은 그대로 반환했음. 결과적으로 0-initialised garbage data 가
        // Unit → NPU 까지 흘러가 noise / 잘못된 inference / 최악의 경우 segfault 유발.
        // 이제는 buffer size 부족이 검출되면 frame 을 회수하고 nullptr 을 반환한다.
        if( map.size < y_sz + u_sz + v_sz ) {
            MLOG_ERROR( "ConvertSampleToAVFrame: buffer size 부족 (map.size=%zu, expected=%zu) — frame drop",
                        static_cast<size_t>( map.size ), y_sz + u_sz + v_sz );
            gst_buffer_unmap( buf, &map );
            av_frame_free( &frame );
            return nullptr;
        }

        std::memcpy( frame->data[ 0 ], src,                y_sz );
        std::memcpy( frame->data[ 1 ], src + y_sz,         u_sz );
        std::memcpy( frame->data[ 2 ], src + y_sz + u_sz,  v_sz );
        gst_buffer_unmap( buf, &map );

        if( GST_BUFFER_PTS_IS_VALID( buf ) ) {
            frame->pts = GST_BUFFER_PTS( buf );
        }

        s_avframe_alive_.fetch_add( 1, std::memory_order_relaxed );
        return std::shared_ptr<AVFrame>( frame, []( AVFrame* f ) {
            av_frame_free( &f );
            s_avframe_alive_.fetch_sub( 1, std::memory_order_relaxed );
        } );
    }

    // leak hunt v4 — process-wide AVFrame alive counter 정의.
    std::atomic<uint64_t> GstRtspReceiver::s_avframe_alive_ { 0 };

    gboolean GstRtspReceiver::OnBusMessage( GstBus* /*bus*/, GstMessage* msg, void* user_data )
    {
        auto* self = static_cast<GstRtspReceiver*>( user_data );
        const GstMessageType mtype = GST_MESSAGE_TYPE( msg );
        const char* mtype_name = GST_MESSAGE_TYPE_NAME( msg );
        const char* src_name = GST_OBJECT_NAME( GST_MESSAGE_SRC( msg ) );
        auto& reg = MetricsRegistry::Instance();
        const std::string cam_id_str = std::to_string( self->cfg_.cam_id );

        // 진단 (debug/gst-rtsp-stale-trace 2026-05-20) — 모든 bus message type 카운트
        reg.IncrementCounter( METRIC_BUS_MSG_TOTAL,
            { { "cam_id", cam_id_str }, { "type", mtype_name ? mtype_name : "?" } }, 1.0 );

        switch( mtype )
        {
            case GST_MESSAGE_ERROR:
            {
                GError* err = nullptr;
                gchar*  dbg = nullptr;
                gst_message_parse_error( msg, &err, &dbg );
                MLOG_ERROR( "GstRtspReceiver[%d] bus ERROR src=%s: %s (debug=%s)",
                    self->cfg_.cam_id, src_name ? src_name : "?",
                    err ? err->message : "?",
                    dbg ? dbg : "?" );
                self->error_count_.fetch_add( 1 );
                reg.IncrementCounter( METRIC_ERRORS_TOTAL, NO_LABELS, 1.0 );
                if( err ) g_error_free( err );
                if( dbg ) g_free( dbg );
                if( self->cfg_.on_error ) {
                    MLOG_WARN( "GstRtspReceiver[%d] on_error 콜백 호출", self->cfg_.cam_id );
                    self->cfg_.on_error();
                }
                break;
            }
            case GST_MESSAGE_WARNING:
            {
                GError* err = nullptr;
                gchar*  dbg = nullptr;
                gst_message_parse_warning( msg, &err, &dbg );
                MLOG_WARN( "GstRtspReceiver[%d] bus WARNING src=%s: %s (debug=%s)",
                    self->cfg_.cam_id, src_name ? src_name : "?",
                    err ? err->message : "?",
                    dbg ? dbg : "?" );
                if( err ) g_error_free( err );
                if( dbg ) g_free( dbg );
                break;
            }
            case GST_MESSAGE_EOS:
                MLOG_WARN( "GstRtspReceiver[%d] EOS received src=%s (stream 종료)",
                    self->cfg_.cam_id, src_name ? src_name : "?" );
                self->reconnect_count_.fetch_add( 1 );
                reg.IncrementCounter( METRIC_RECONNECT_TOTAL, NO_LABELS, 1.0 );
                if( self->cfg_.on_eos ) {
                    MLOG_WARN( "GstRtspReceiver[%d] on_eos 콜백 호출", self->cfg_.cam_id );
                    self->cfg_.on_eos();
                }
                break;
            case GST_MESSAGE_STATE_CHANGED:
            {
                // pipeline element 의 state 변경만 (모든 element 의 변경은 너무 verbose).
                if( GST_MESSAGE_SRC( msg ) == GST_OBJECT( self->pipeline_ ) ) {
                    GstState old_st, new_st, pend_st;
                    gst_message_parse_state_changed( msg, &old_st, &new_st, &pend_st );
                    MLOG_INFO( "GstRtspReceiver[%d] pipeline STATE_CHANGED: %s → %s (pending=%s)",
                        self->cfg_.cam_id,
                        gst_element_state_get_name( old_st ),
                        gst_element_state_get_name( new_st ),
                        gst_element_state_get_name( pend_st ) );
                }
                break;
            }
            case GST_MESSAGE_STREAM_STATUS:
            {
                GstStreamStatusType ssm_type;
                GstElement* owner = nullptr;
                gst_message_parse_stream_status( msg, &ssm_type, &owner );
                // verbose, debug 만. cam stuck 분석 시 stream 의 enter/leave 패턴 보기.
                MLOG_DEBUG( "GstRtspReceiver[%d] STREAM_STATUS type=%d owner=%s",
                    self->cfg_.cam_id, (int) ssm_type,
                    owner ? GST_OBJECT_NAME( owner ) : "?" );
                break;
            }
            case GST_MESSAGE_ELEMENT:
            {
                const GstStructure* s = gst_message_get_structure( msg );
                if( s && gst_structure_has_name( s, "GstRTSPSrcTimeout" ) ) {
                    gint cause_int = -1;
                    const GValue* cause_val = gst_structure_get_value( s, "cause" );
                    if( cause_val ) {
                        if( G_VALUE_HOLDS_ENUM( cause_val ) )       cause_int = g_value_get_enum( cause_val );
                        else if( G_VALUE_HOLDS_INT ( cause_val ) )  cause_int = g_value_get_int ( cause_val );
                    }
                    constexpr gint RTSPSRC_TIMEOUT_CAUSE_RTCP = 0;
                    if( cause_int == RTSPSRC_TIMEOUT_CAUSE_RTCP ) {
                        // 측정 (experiment/rtcp-timeout-measure 2026-05-21):
                        //   발생 빈도만 카운트. behavior 변경 없음 (여전히 reconnect 안 함).
                        //   목적: RTCP timeout 이 정상 운영 중 얼마나 자주 발생하는지 → fix 방향 결정.
                        reg.IncrementCounter( METRIC_RTCP_TIMEOUT_TOTAL, { { "cam_id", cam_id_str } }, 1.0 );
                        MLOG_WARN( "GstRtspReceiver[%d] GstRTSPSrcTimeout cause=RTCP (측정 — reconnect 안 함)",
                            self->cfg_.cam_id );
                    } else {
                        MLOG_WARN( "GstRtspReceiver[%d] GstRTSPSrcTimeout cause=%d — reconnect 트리거",
                            self->cfg_.cam_id, cause_int );
                        self->reconnect_count_.fetch_add( 1 );
                        reg.IncrementCounter( METRIC_RECONNECT_TOTAL, NO_LABELS, 1.0 );
                        if( self->cfg_.on_timeout ) {
                            MLOG_WARN( "GstRtspReceiver[%d] on_timeout 콜백 호출", self->cfg_.cam_id );
                            self->cfg_.on_timeout();
                        }
                    }
                } else if( s ) {
                    // 다른 element message 도 진단 — cam stuck 시 어떤 element 가 message 보내는지 확인.
                    const gchar* name = gst_structure_get_name( s );
                    MLOG_DEBUG( "GstRtspReceiver[%d] ELEMENT msg src=%s structure=%s",
                        self->cfg_.cam_id, src_name ? src_name : "?", name ? name : "?" );
                }
                break;
            }
            case GST_MESSAGE_BUFFERING:
            case GST_MESSAGE_LATENCY:
            case GST_MESSAGE_ASYNC_DONE:
            case GST_MESSAGE_NEW_CLOCK:
                // 일반적 — 1회 log 만.
                MLOG_DEBUG( "GstRtspReceiver[%d] bus msg type=%s src=%s",
                    self->cfg_.cam_id, mtype_name ? mtype_name : "?", src_name ? src_name : "?" );
                break;
            default:
                // unknown message type — 진단 시 cam stuck 의 단서일 수 있음.
                MLOG_INFO( "GstRtspReceiver[%d] bus msg UNKNOWN type=%s(%d) src=%s",
                    self->cfg_.cam_id, mtype_name ? mtype_name : "?",
                    (int) mtype, src_name ? src_name : "?" );
                break;
        }
        return TRUE;
    }

} // namespace MGEN
