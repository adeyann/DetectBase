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

#include <cstring>

namespace MGEN
{
    namespace
    {
        constexpr const char* METRIC_FRAMES_TOTAL    = "detectbase_gst_rtsp_frames_total";
        constexpr const char* METRIC_RECONNECT_TOTAL = "detectbase_gst_rtsp_reconnect_total";
        constexpr const char* METRIC_ERRORS_TOTAL    = "detectbase_gst_rtsp_errors_total";

        const std::map<std::string, std::string> NO_LABELS;

        bool g_metrics_registered = false;

        void RegisterMetricsOnce() noexcept
        {
            if( g_metrics_registered ) return;
            auto& reg = MetricsRegistry::Instance();
            reg.RegisterCounter( METRIC_FRAMES_TOTAL,    "GStreamer RTSP frames received and decoded" );
            reg.RegisterCounter( METRIC_RECONNECT_TOTAL, "GStreamer RTSP reconnect attempts" );
            reg.RegisterCounter( METRIC_ERRORS_TOTAL,    "GStreamer RTSP bus error events" );
            g_metrics_registered = true;
        }
    } // anonymous

    GstRtspReceiver::GstRtspReceiver( const Config& cfg, FrameCallback cb ) noexcept
        : cfg_( cfg )
        , cb_ ( std::move( cb ) )
    {
        RegisterMetricsOnce();
        MLOG_INFO( "GstRtspReceiver 생성 — url=%s latency=%dms (single-pipeline)", cfg_.url.c_str(), cfg_.latency_ms );
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
                "  ! rtph264depay ! h264parse "
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
                "  ! rtph264depay ! h264parse "
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

        return true;
    }

    void GstRtspReceiver::TeardownPipeline()
    {
        if( pipeline_ ) {
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
        if( !pipeline_ ) return false;

        reset_source_count_.fetch_add( 1, std::memory_order_relaxed );
        MLOG_INFO( "ResetSourceOnly — pipeline destroy/rebuild (single)" );

        TeardownPipeline();

        if( !BuildPipeline() ) {
            MLOG_ERROR( "ResetSourceOnly — BuildPipeline 실패" );
            return false;
        }
        GstStateChangeReturn r = gst_element_set_state( pipeline_, GST_STATE_PLAYING );
        if( r == GST_STATE_CHANGE_FAILURE ) {
            MLOG_ERROR( "ResetSourceOnly — PLAYING 실패" );
            TeardownPipeline();
            return false;
        }
        MLOG_INFO( "ResetSourceOnly OK" );
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

        switch( GST_MESSAGE_TYPE( msg ) )
        {
            case GST_MESSAGE_ERROR:
            {
                GError* err = nullptr;
                gchar*  dbg = nullptr;
                gst_message_parse_error( msg, &err, &dbg );
                MLOG_ERROR( "GstRtspReceiver bus ERROR: %s (debug=%s)",
                    err ? err->message : "?",
                    dbg ? dbg : "?" );
                self->error_count_.fetch_add( 1 );
                MetricsRegistry::Instance().IncrementCounter( METRIC_ERRORS_TOTAL, NO_LABELS, 1.0 );
                if( err ) g_error_free( err );
                if( dbg ) g_free( dbg );
                if( self->cfg_.on_error ) self->cfg_.on_error();
                break;
            }
            case GST_MESSAGE_EOS:
                MLOG_WARN( "GstRtspReceiver EOS received (stream 종료)" );
                self->reconnect_count_.fetch_add( 1 );
                MetricsRegistry::Instance().IncrementCounter( METRIC_RECONNECT_TOTAL, NO_LABELS, 1.0 );
                if( self->cfg_.on_eos ) self->cfg_.on_eos();
                break;
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
                        static thread_local bool logged_once = false;
                        if( !logged_once ) {
                            MLOG_INFO( "GstRTSPSrcTimeout cause=RTCP — stream 정상, reconnect 안 함 (이후 silent)" );
                            logged_once = true;
                        }
                    } else {
                        MLOG_WARN( "GstRTSPSrcTimeout cause=%d — reconnect 트리거", cause_int );
                        self->reconnect_count_.fetch_add( 1 );
                        MetricsRegistry::Instance().IncrementCounter( METRIC_RECONNECT_TOTAL, NO_LABELS, 1.0 );
                        if( self->cfg_.on_timeout ) self->cfg_.on_timeout();
                    }
                }
                break;
            }
            default:
                break;
        }
        return TRUE;
    }

} // namespace MGEN
