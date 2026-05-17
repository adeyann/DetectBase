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
            int  timeout_us          = 2000000;
            int  tcp_timeout_us      = 5000000;
            bool do_retransmission   = true;
            bool keepalive           = true;
            int  appsink_max_buffers = 2;

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

    private:
        static GstFlowReturn OnNewSample   ( GstAppSink* sink, void* user_data );
        static GstFlowReturn OnNewRawSample( GstAppSink* sink, void* user_data );
        static gboolean      OnBusMessage  ( GstBus* bus, GstMessage* msg, void* user_data );

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

        std::atomic<bool>     running_         { false };
        std::atomic<uint64_t> frame_count_     { 0 };
        std::atomic<uint64_t> reconnect_count_ { 0 };
        std::atomic<uint64_t> error_count_     { 0 };
    };

} // namespace MGEN
