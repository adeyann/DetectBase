/**
 * @file GstRtspReceiver.h
 * @brief GstRtspReceiver — GStreamer 기반 RTSP 단일 pipeline (avdec_h264 / I420).
 *
 * @details
 * Pipeline:
 *   rtspsrc → rtph264depay → h264parse → avdec_h264 → videoconvert → "video/x-raw,format=I420" → appsink
 *
 * 책임:
 *   - 외부 RTSP URL 연결, H.264 디코드, AVFrame 으로 콜백
 *   - bus watch + reconnect (Critical C3)
 *
 * Critical:
 *   - C2 (AVFrame deleter): shared_ptr<AVFrame> 의 deleter 는 av_frame_free 필수.
 *   - C3 (rtspsrc reconnect): rtspsrc 는 자동 reconnect 안 함. ERROR/timeout 시 pipeline NULL → restart.
 *
 * 사용:
 *   GstRtspReceiver::Config cfg;
 *   cfg.url = "rtsp://192.168.1.72:555/658";
 *   GstRtspReceiver receiver( cfg, [](std::shared_ptr<AVFrame> f){ ... } );
 *   receiver.Start(); ... receiver.Stop();
 *
 * 참조: logs/GSTREAMER_DEEP_REVIEW.md §2 (rtspsrc), §3 (Bridge), §C2~C3
 */

#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>

#include <gst/gst.h>
#include <gst/app/gstappsink.h>

struct AVFrame;

namespace MGEN
{
    class GstRtspReceiver
    {
    public:
        struct Config
        {
            std::string url;
            std::string user_id;
            std::string user_pw;
            int  latency_ms          = 200;
            int  timeout_us          = 2000000;  // 원복 (2026-05-21): 2s. RTCP timeout 은 timeout property 와 무관 확인됨 (EOS cycle 종속)
            int  tcp_timeout_us      = 5000000;
            bool do_retransmission   = true;
            bool keepalive           = true;
            int  appsink_max_buffers = 2;

            /// 진단용 cam_id (log/metric 에 명시). 0 = 미지정.
            int  cam_id = 0;

            std::function<void()> on_error;
            std::function<void()> on_timeout;
            std::function<void()> on_eos;

            /// video forward 활성화 — true 면 raw RTP H.264 packet 분기 추가.
            bool enable_raw_passthrough = false;
        };

        using FrameCallback     = std::function<void(std::shared_ptr<AVFrame>)>;
        using RawPacketCallback = std::function<void(const uint8_t* data, size_t size)>;

        GstRtspReceiver( const Config& cfg, FrameCallback cb ) noexcept;
        ~GstRtspReceiver();

        void SetRawPacketCallback( RawPacketCallback cb ) noexcept;

        GstRtspReceiver( const GstRtspReceiver& )            = delete;
        GstRtspReceiver& operator=( const GstRtspReceiver& ) = delete;
        GstRtspReceiver( GstRtspReceiver&& )                 = delete;
        GstRtspReceiver& operator=( GstRtspReceiver&& )      = delete;

        bool Start() noexcept;
        void Stop()  noexcept;

        /// pipeline NULL → 새 pipeline (reconnect 용).
        bool ResetSourceOnly() noexcept;

        bool IsRunning() const noexcept { return running_.load(); }

        uint64_t GetFrameCount()     const noexcept { return frame_count_.load();     }
        uint64_t GetReconnectCount() const noexcept { return reconnect_count_.load(); }
        uint64_t GetErrorCount()     const noexcept { return error_count_.load();     }

        // GStreamer appsink (frame I420 sink) 의 queued buffer 개수. leak hunt — GStreamer pipeline 내부 누적 측정.
        uint32_t GetAppSinkQueuedBuffers() const noexcept;

        // leak hunt v4 — AVFrame shared_ptr alive count (process-wide, deleter wrap). frame 1개 ~3MB.
        static uint64_t GetAvFrameAliveCount() noexcept { return s_avframe_alive_.load(); }

        // leak hunt v4 — ResetSourceOnly 호출 누적 카운터 (EOS reconn 빈도 측정 + 매 reconn 시 leak 감시).
        uint64_t GetResetSourceCount() const noexcept { return reset_source_count_.load(); }

        // 진단 (debug/gst-rtsp-stale-trace 2026-05-20) — 마지막 frame 수신 시각 (steady_clock ns).
        int64_t GetLastFrameNs() const noexcept { return last_frame_ns_.load(); }

    private:
        static GstFlowReturn OnNewSample   ( GstAppSink* sink, void* user_data );
        static GstFlowReturn OnNewRawSample( GstAppSink* sink, void* user_data );
        static gboolean      OnBusMessage  ( GstBus* bus, GstMessage* msg, void* user_data );
        // 측정 (2026-05-21): depay src pad probe — RTP/pre-decode buffer flow per cam.
        //   stuck 시 이 counter 가 멈추면 data 가 decode 전(rtspsrc/socket)에서 정지,
        //   계속 증가하면 decode 단계 stall. decoded counter(OnNewSample)와 비교해 stuck 위치 확정.
        static GstPadProbeReturn OnDepayBufferProbe( GstPad* pad, GstPadProbeInfo* info, void* user_data );
        // 검증 (2026-05-21): depay SINK probe — rtspsrc 가 depay 로 내보내는 buffer.
        //   stuck 시 rtp_in=0 이면 block 이 rtspsrc 내부 (udpsrc/jitterbuffer), >0 이면 depay+ 단계.
        static GstPadProbeReturn OnRtpInProbe( GstPad* pad, GstPadProbeInfo* info, void* user_data );
        // 검증 (2026-05-21): rtpjitterbuffer stats 추적 — packet loss / RTX 대기 정지 확인.
        static void     OnNewManager     ( GstElement* rtspsrc, GstElement* manager, void* user_data );
        static void     OnNewJitterbuffer( GstElement* rtpbin, GstElement* jb, guint session, guint ssrc, void* user_data );
        static gboolean OnJitterStatsTimer( void* user_data );

        bool BuildPipeline();
        void TeardownPipeline();
        void MainLoopThreadFunc();
        std::shared_ptr<AVFrame> ConvertSampleToAVFrame( GstSample* sample ) noexcept;

        Config             cfg_;
        FrameCallback      cb_;
        RawPacketCallback  raw_cb_;

        GstElement*     pipeline_     = nullptr;
        GstAppSink*     appsink_      = nullptr;
        GstAppSink*     raw_appsink_  = nullptr;
        GMainLoop*      loop_         = nullptr;
        std::thread     loop_thread_;
        guint           bus_watch_id_ = 0;

        // 검증 (2026-05-21): rtpjitterbuffer stats 추적용
        GstElement*     jitterbuffer_   = nullptr;  // weak ref, owner: pipeline (rtpbin 내부)
        guint           jitter_timer_id_ = 0;

        std::atomic<bool>     running_         { false };
        std::atomic<uint64_t> frame_count_     { 0 };
        std::atomic<uint64_t> reconnect_count_ { 0 };
        std::atomic<uint64_t> error_count_     { 0 };

        // leak hunt v4 — ResetSourceOnly 호출 누적.
        std::atomic<uint64_t> reset_source_count_ { 0 };

        // 진단 (debug/gst-rtsp-stale-trace 2026-05-20) — 마지막 frame 수신 시각 (steady_clock ns).
        std::atomic<int64_t>  last_frame_ns_      { 0 };

        // leak hunt v4 — process-wide AVFrame alive count (ConvertSampleToAVFrame deleter wrap).
        static std::atomic<uint64_t> s_avframe_alive_;
    };

} // namespace MGEN
