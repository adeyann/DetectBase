/**
 * @file GstRtspReceiver.cpp
 * @brief GstRtspReceiver — Option A (partial reset) — source/decode side 분리.
 *
 * Pipeline 구성:
 *   SOURCE-SIDE (ResetSourceOnly 마다 destroy/recreate):
 *     rtspsrc → rtph264depay → h264parse [ → tee → raw_queue → rtph264pay → raw_appsink ]
 *   BOUNDARY:
 *     parse.src (또는 raw_passthrough=true 시 tee.src_%u) → decode_queue.sink
 *   DECODE-SIDE (Stop 까지 영구 보존):
 *     decode_queue (leaky=downstream) → decoder (avdec_h264 → 추후 mppvideodec) → videoconvert
 *                                     → capsfilter(I420) → appsink
 *
 * 설계 목적: mppvideodec 의 internal hardware DMA buffer 가 매 reconnect 마다 재할당돼
 *   leak 으로 누적되던 이전 시도 (2026-05-15 rollback) 의 해결책. decode-side 가 영구
 *   보존되므로 reset 시 DMA buffer 재할당 없음.
 *
 * 이전 history:
 *   2026-05-15: 2-pipeline architecture + mppvideodec → leak 발생 → rollback 후 avdec_h264 단일 pipeline.
 *   2026-05-25: feature/mpp-integration branch — Option A 로 재시도.
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
        // Option A — partial reset 구조 :
        //   pipeline_ (container) + BuildDecodeSide (Stop 까지 영구) + BuildSourceSide (ResetSourceOnly 마다 swap)
        //   bus watch 는 pipeline_ 에 묶임 (영구).
        pipeline_ = gst_pipeline_new( nullptr );
        if( !pipeline_ ) {
            MLOG_ERROR( "GstRtspReceiver[%d] gst_pipeline_new 실패", cfg_.cam_id );
            return false;
        }

        GstBus* bus = gst_pipeline_get_bus( GST_PIPELINE( pipeline_ ) );
        bus_watch_id_ = gst_bus_add_watch( bus, &GstRtspReceiver::OnBusMessage, this );
        gst_object_unref( bus );

        if( !BuildDecodeSide() ) {
            MLOG_ERROR( "GstRtspReceiver[%d] BuildDecodeSide 실패", cfg_.cam_id );
            TeardownPipeline();
            return false;
        }

        if( !BuildSourceSide() ) {
            MLOG_ERROR( "GstRtspReceiver[%d] BuildSourceSide 실패", cfg_.cam_id );
            TeardownPipeline();
            return false;
        }

        // 검증 (2026-05-21): jitterbuffer stats timer (매 EOS rebuild 마다 재등록).
        //   ResetSourceOnly 가 TeardownSourceSide 안에서 timer 제거 후 BuildSourceSide 다음 단계에서 재추가.
        jitter_timer_id_ = g_timeout_add( 2000, &GstRtspReceiver::OnJitterStatsTimer, this );

        MLOG_INFO( "GstRtspReceiver[%d] pipeline 구성 완료 (partial-reset 지원, raw_passthrough=%s)",
            cfg_.cam_id, cfg_.enable_raw_passthrough ? "ON" : "OFF" );
        return true;
    }

    bool GstRtspReceiver::BuildDecodeSide()
    {
        // DECODE-SIDE (영구 보존) :
        //   decode_queue (boundary entry) → decoder (avdec_h264, mppvideodec 으로 swap 예정)
        //                                 → videoconvert → capsfilter(I420) → appsink
        decode_queue_ = gst_element_factory_make( "queue",        "decodeq" );
        decoder_      = gst_element_factory_make( "avdec_h264",   "dec"     );
        videoconvert_ = gst_element_factory_make( "videoconvert", "conv"    );
        capsfilter_   = gst_element_factory_make( "capsfilter",   "caps"    );
        GstElement* sink_elem = gst_element_factory_make( "appsink", "sink" );

        if( !decode_queue_ || !decoder_ || !videoconvert_ || !capsfilter_ || !sink_elem ) {
            MLOG_ERROR( "GstRtspReceiver[%d] DecodeSide element_factory_make 실패", cfg_.cam_id );
            return false;
        }

        // decode_queue (boundary entry): leaky=downstream, decoder backpressure 흡수.
        //   기존 (gst_parse_launch 단일 desc) 의 "queue name=decodeq max-size-buffers=4 leaky=downstream" 동등.
        g_object_set( decode_queue_,
            "max-size-buffers", 4u,
            "leaky",            2,  // 2 = downstream
            nullptr );

        // capsfilter: I420 강제 (downstream NV12 우려 차단)
        GstCaps* i420_caps = gst_caps_new_simple( "video/x-raw",
            "format", G_TYPE_STRING, "I420",
            nullptr );
        g_object_set( capsfilter_, "caps", i420_caps, nullptr );
        gst_caps_unref( i420_caps );

        // appsink callback 설정
        g_object_set( sink_elem,
            "emit-signals", TRUE,
            "sync",         FALSE,
            "max-buffers",  static_cast<guint>( cfg_.appsink_max_buffers ),
            "drop",         TRUE,
            nullptr );
        appsink_ = GST_APP_SINK( sink_elem );  // non-owning cast, bin 이 소유
        GstAppSinkCallbacks cbs {};
        cbs.new_sample = &GstRtspReceiver::OnNewSample;
        gst_app_sink_set_callbacks( appsink_, &cbs, this, nullptr );

        gst_bin_add_many( GST_BIN( pipeline_ ),
            decode_queue_, decoder_, videoconvert_, capsfilter_, sink_elem,
            nullptr );

        if( !gst_element_link_many( decode_queue_, decoder_, videoconvert_, capsfilter_, sink_elem, nullptr ) ) {
            MLOG_ERROR( "GstRtspReceiver[%d] DecodeSide link_many 실패", cfg_.cam_id );
            return false;
        }
        return true;
    }

    bool GstRtspReceiver::BuildSourceSide()
    {
        // SOURCE-SIDE (매 reset 마다 swap) :
        //   rtspsrc(dyn pad) → rtph264depay → h264parse → [tee → raw 분기 if raw_passthrough]
        //   boundary: parse.src (또는 tee.src_%u) → decode_queue.sink
        rtspsrc_ = gst_element_factory_make( "rtspsrc",       "src"   );
        depay_   = gst_element_factory_make( "rtph264depay",  "depay" );
        parse_   = gst_element_factory_make( "h264parse",     "parse" );

        if( !rtspsrc_ || !depay_ || !parse_ ) {
            MLOG_ERROR( "GstRtspReceiver[%d] SourceSide element_factory_make 실패", cfg_.cam_id );
            return false;
        }

        // rtspsrc 설정 — 기존 desc 의 property 동등 적용.
        g_object_set( rtspsrc_,
            "location",          cfg_.url.c_str(),
            "latency",           cfg_.latency_ms,
            "timeout",           static_cast<guint64>( cfg_.timeout_us ),
            "tcp-timeout",       static_cast<guint64>( cfg_.tcp_timeout_us ),
            "do-retransmission", cfg_.do_retransmission ? TRUE : FALSE,
            "udp-buffer-size",   2097152,
            nullptr );
        if( !cfg_.user_id.empty() ) {
            g_object_set( rtspsrc_,
                "user-id", cfg_.user_id.c_str(),
                "user-pw", cfg_.user_pw.c_str(),
                nullptr );
        }

        gst_bin_add_many( GST_BIN( pipeline_ ), rtspsrc_, depay_, parse_, nullptr );

        // depay → parse 정적 link
        if( !gst_element_link( depay_, parse_ ) ) {
            MLOG_ERROR( "GstRtspReceiver[%d] depay → parse link 실패", cfg_.cam_id );
            return false;
        }

        // rtspsrc dynamic pad → depay.sink (pad-added 신호)
        g_signal_connect( rtspsrc_, "pad-added",
            G_CALLBACK( &GstRtspReceiver::OnRtspsrcPadAdded ), this );
        // rtpbin manager (jitterbuffer 캡처)
        g_signal_connect( rtspsrc_, "new-manager",
            G_CALLBACK( &GstRtspReceiver::OnNewManager ), this );

        // depay src buffer probe (METRIC_DEPAY_BUFFER_TOTAL)
        GstPad* depay_src = gst_element_get_static_pad( depay_, "src" );
        if( depay_src ) {
            gst_pad_add_probe( depay_src, GST_PAD_PROBE_TYPE_BUFFER,
                &GstRtspReceiver::OnDepayBufferProbe, this, nullptr );
            gst_object_unref( depay_src );
        }

        // boundary 결정 — 기본 parse, raw_passthrough 면 tee.src_%u
        GstElement* boundary_src_elem = parse_;

        if( cfg_.enable_raw_passthrough ) {
            // tee 분기: parse → tee → [boundary_src → decode_queue], [raw_queue → raw_pay → raw_sink]
            raw_tee_   = gst_element_factory_make( "tee",          "tee_h264"  );
            raw_queue_ = gst_element_factory_make( "queue",        "raw_queue" );
            raw_pay_   = gst_element_factory_make( "rtph264pay",   "raw_pay"   );
            GstElement* raw_sink_elem = gst_element_factory_make( "appsink", "raw_sink" );

            if( !raw_tee_ || !raw_queue_ || !raw_pay_ || !raw_sink_elem ) {
                MLOG_ERROR( "GstRtspReceiver[%d] Raw branch element_factory_make 실패", cfg_.cam_id );
                return false;
            }

            g_object_set( raw_queue_,
                "max-size-buffers", 8u,
                "leaky",            2,
                nullptr );
            g_object_set( raw_pay_,
                "pt",              96,
                "config-interval", 1,
                nullptr );
            g_object_set( raw_sink_elem,
                "emit-signals", TRUE,
                "sync",         FALSE,
                "max-buffers",  20u,
                "drop",         TRUE,
                nullptr );
            raw_appsink_ = GST_APP_SINK( raw_sink_elem );  // non-owning cast
            GstAppSinkCallbacks raw_cbs {};
            raw_cbs.new_sample = &GstRtspReceiver::OnNewRawSample;
            gst_app_sink_set_callbacks( raw_appsink_, &raw_cbs, this, nullptr );

            gst_bin_add_many( GST_BIN( pipeline_ ),
                raw_tee_, raw_queue_, raw_pay_, raw_sink_elem, nullptr );

            if( !gst_element_link( parse_, raw_tee_ ) ) {
                MLOG_ERROR( "GstRtspReceiver[%d] parse → tee link 실패", cfg_.cam_id );
                return false;
            }
            if( !gst_element_link_many( raw_tee_, raw_queue_, raw_pay_, raw_sink_elem, nullptr ) ) {
                MLOG_ERROR( "GstRtspReceiver[%d] tee → raw branch link 실패", cfg_.cam_id );
                return false;
            }
            boundary_src_elem = raw_tee_;
        }

        // BOUNDARY: boundary_src_elem → decode_queue (= decode-side preserve entry)
        if( !gst_element_link( boundary_src_elem, decode_queue_ ) ) {
            MLOG_ERROR( "GstRtspReceiver[%d] boundary link 실패 (%s → decode_queue)",
                cfg_.cam_id, boundary_src_elem == raw_tee_ ? "tee" : "parse" );
            return false;
        }

        return true;
    }

    void GstRtspReceiver::OnRtspsrcPadAdded( GstElement* /*src*/, GstPad* new_pad, void* user_data )
    {
        auto* self = static_cast<GstRtspReceiver*>( user_data );
        if( !self->depay_ ) return;

        GstPad* depay_sink = gst_element_get_static_pad( self->depay_, "sink" );
        if( !depay_sink ) return;

        if( gst_pad_is_linked( depay_sink ) ) {
            gst_object_unref( depay_sink );
            return;
        }

        // application/x-rtp media=video 만 link (audio 등 skip)
        GstCaps* caps = gst_pad_get_current_caps( new_pad );
        bool linked = false;
        if( caps ) {
            const GstStructure* s = gst_caps_get_structure( caps, 0 );
            const gchar* media = gst_structure_get_string( s, "media" );
            if( !media || std::strcmp( media, "video" ) == 0 ) {
                GstPadLinkReturn r = gst_pad_link( new_pad, depay_sink );
                if( r == GST_PAD_LINK_OK ) {
                    linked = true;
                } else {
                    MLOG_WARN( "GstRtspReceiver[%d] pad_link 실패 ret=%d", self->cfg_.cam_id, r );
                }
            }
            gst_caps_unref( caps );
        }
        if( !linked ) {
            // caps 가 아직 없을 수도 (legacy rtspsrc) → caps 무관 link 시도
            gst_pad_link( new_pad, depay_sink );
        }
        gst_object_unref( depay_sink );
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

    void GstRtspReceiver::TeardownSourceSide()
    {
        // jitter timer 제거 (jitterbuffer 가 곧 소멸)
        if( jitter_timer_id_ ) {
            g_source_remove( jitter_timer_id_ );
            jitter_timer_id_ = 0;
        }
        jitterbuffer_ = nullptr;  // weak ref — rtspsrc 가 곧 destroy

        // boundary unlink (parse 또는 tee → decode_queue)
        if( decode_queue_ ) {
            GstElement* bs = cfg_.enable_raw_passthrough ? raw_tee_ : parse_;
            if( bs ) {
                gst_element_unlink( bs, decode_queue_ );
            }
        }

        // raw_appsink 는 non-owning cast — element 자체를 bin_remove 로 destroy.
        raw_appsink_ = nullptr;
        GstElement* raw_sink_elem = nullptr;
        if( cfg_.enable_raw_passthrough && pipeline_ ) {
            raw_sink_elem = gst_bin_get_by_name( GST_BIN( pipeline_ ), "raw_sink" );
        }

        auto null_and_remove = [this]( GstElement** elem ) {
            if( *elem ) {
                gst_element_set_state( *elem, GST_STATE_NULL );
                if( pipeline_ ) {
                    gst_bin_remove( GST_BIN( pipeline_ ), *elem );  // bin_remove 가 unref
                }
                *elem = nullptr;
            }
        };

        null_and_remove( &rtspsrc_   );
        null_and_remove( &depay_     );
        null_and_remove( &parse_     );
        null_and_remove( &raw_tee_   );
        null_and_remove( &raw_queue_ );
        null_and_remove( &raw_pay_   );

        if( raw_sink_elem ) {
            gst_element_set_state( raw_sink_elem, GST_STATE_NULL );
            gst_bin_remove( GST_BIN( pipeline_ ), raw_sink_elem );  // bin 의 ref release
            gst_object_unref( raw_sink_elem );                       // gst_bin_get_by_name 의 ref release
        }
    }

    void GstRtspReceiver::TeardownDecodeSide()
    {
        // appsink 는 non-owning cast — element 를 bin_remove 로 destroy.
        appsink_ = nullptr;
        GstElement* sink_elem = nullptr;
        if( pipeline_ ) {
            sink_elem = gst_bin_get_by_name( GST_BIN( pipeline_ ), "sink" );
        }

        auto null_and_remove = [this]( GstElement** elem ) {
            if( *elem ) {
                gst_element_set_state( *elem, GST_STATE_NULL );
                if( pipeline_ ) {
                    gst_bin_remove( GST_BIN( pipeline_ ), *elem );
                }
                *elem = nullptr;
            }
        };

        null_and_remove( &decode_queue_ );
        null_and_remove( &decoder_      );
        null_and_remove( &videoconvert_ );
        null_and_remove( &capsfilter_   );

        if( sink_elem ) {
            gst_element_set_state( sink_elem, GST_STATE_NULL );
            gst_bin_remove( GST_BIN( pipeline_ ), sink_elem );
            gst_object_unref( sink_elem );
        }
    }

    void GstRtspReceiver::TeardownPipeline()
    {
        if( pipeline_ ) {
            // bus watch 제거 (pipeline 영구 자원)
            if( bus_watch_id_ ) {
                g_source_remove( bus_watch_id_ );
                bus_watch_id_ = 0;
            }
            // Source-side teardown (jitter timer + 소스 elements 모두)
            TeardownSourceSide();
            // Decode-side teardown (보존됐던 elements)
            TeardownDecodeSide();
            // pipeline 자체 NULL → unref
            gst_element_set_state( pipeline_, GST_STATE_NULL );
            GstState st;
            gst_element_get_state( pipeline_, &st, nullptr, 5 * GST_SECOND );
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
        // Option A — PARTIAL RESET :
        //   1. TeardownSourceSide  — source-side elements 만 NULL + bin_remove (decode-side 보존)
        //   2. BuildSourceSide     — source 재생성 + decode_queue 와 boundary link
        //   3. sync_state_with_parent — pipeline 이 PLAYING 이라면 새 elements 도 PLAYING 으로
        //   4. jitter_timer 재등록 — 새 jitterbuffer 캡처 대비
        //   mppvideodec 가 decode-side 에 들어가면 DMA buffer 영구 보존 → leak 회피.
        const auto t_start = std::chrono::steady_clock::now();
        auto& reg = MetricsRegistry::Instance();
        const std::string cam_id_str = std::to_string( cfg_.cam_id );

        if( !pipeline_ ) {
            MLOG_WARN( "ResetSourceOnly[%d] — pipeline_ is nullptr (skipped)", cfg_.cam_id );
            reg.IncrementCounter( METRIC_RESET_ATTEMPT, { { "cam_id", cam_id_str }, { "result", "no_pipeline" } }, 1.0 );
            return false;
        }

        reset_source_count_.fetch_add( 1, std::memory_order_relaxed );
        MLOG_INFO( "ResetSourceOnly[%d] 진입 — PARTIAL reset (decode-side preserved), reset_cnt=%lu",
            cfg_.cam_id, (unsigned long) reset_source_count_.load() );
        reg.IncrementCounter( METRIC_RESET_ATTEMPT, { { "cam_id", cam_id_str }, { "result", "enter" } }, 1.0 );

        TeardownSourceSide();

        if( !BuildSourceSide() ) {
            MLOG_ERROR( "ResetSourceOnly[%d] — BuildSourceSide 실패", cfg_.cam_id );
            reg.IncrementCounter( METRIC_RESET_ATTEMPT, { { "cam_id", cam_id_str }, { "result", "build_fail" } }, 1.0 );
            return false;
        }

        // 새 source elements 의 state 를 parent(=pipeline=PLAYING) 와 동기화.
        //   sync_state_with_parent 가 PLAYING 으로 추진. 실패하면 stream 안 흐름.
        bool sync_ok = true;
        if( rtspsrc_   && !gst_element_sync_state_with_parent( rtspsrc_   ) ) sync_ok = false;
        if( depay_     && !gst_element_sync_state_with_parent( depay_     ) ) sync_ok = false;
        if( parse_     && !gst_element_sync_state_with_parent( parse_     ) ) sync_ok = false;
        if( raw_tee_   && !gst_element_sync_state_with_parent( raw_tee_   ) ) sync_ok = false;
        if( raw_queue_ && !gst_element_sync_state_with_parent( raw_queue_ ) ) sync_ok = false;
        if( raw_pay_   && !gst_element_sync_state_with_parent( raw_pay_   ) ) sync_ok = false;
        if( cfg_.enable_raw_passthrough ) {
            GstElement* rsink = gst_bin_get_by_name( GST_BIN( pipeline_ ), "raw_sink" );
            if( rsink ) {
                if( !gst_element_sync_state_with_parent( rsink ) ) sync_ok = false;
                gst_object_unref( rsink );
            }
        }
        if( !sync_ok ) {
            MLOG_WARN( "ResetSourceOnly[%d] — 일부 element state sync 실패", cfg_.cam_id );
            // 치명적 아님 — 일부 element 가 NULL/READY 머물 수 있으나 GStreamer 가 자체 회복 시도.
        }

        // jitter stats timer 재등록 (TeardownSourceSide 에서 제거됨)
        jitter_timer_id_ = g_timeout_add( 2000, &GstRtspReceiver::OnJitterStatsTimer, this );

        const auto t_end = std::chrono::steady_clock::now();
        const int64_t dur_us = std::chrono::duration_cast<std::chrono::microseconds>( t_end - t_start ).count();
        MLOG_INFO( "ResetSourceOnly[%d] OK — PARTIAL reset duration=%ldus", cfg_.cam_id, (long) dur_us );
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
