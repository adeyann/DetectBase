/**
 * @file GstRtspClient.h
 * @brief Per-camera GStreamer RTSP 통합 client.
 *
 * @details
 * 책임:
 *   - GstRtspReceiver 생성/관리 (RTSP 수신 + H.264 디코드)
 *   - Critical C3 자동 reconnect (bus ERROR/Timeout/EOS 시 exponential backoff 후 재시작)
 *   - 수신 AVFrame 을 SafeQueue<shared_ptr<AVFrame>> 에 enqueue (IOStreamManager 호환)
 *   - graceful 종료 (Stop 시 reconnect 즉시 중단)
 *
 * 사용:
 *   auto queue = std::make_shared<SafeQueue<shared_ptr<AVFrame>>>();
 *   queue->SetMaxSize(10);
 *
 *   GstRtspClient::Config cfg;
 *   cfg.cam_id = 658;
 *   cfg.rtsp_url = "rtsp://192.168.1.72:555/658";
 *
 *   GstRtspClient client(cfg, queue);
 *   client.Start();
 *   ...
 *   client.Stop();
 *
 * 참조: logs/GSTREAMER_DEEP_REVIEW.md §2.1 (Critical C3 reconnect 로직)
 */

#pragma once

#include "GstRtspReceiver.h"
#include "SafeQueue.h"  // BasicLibs/core/structure/SafeQueue.h

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

struct AVFrame;

namespace MGEN
{
    class GstRtspClient
    {
    public:
        struct Config
        {
            int         cam_id   = 0;            ///< 카메라 ID (메트릭 라벨용)
            std::string rtsp_url;                ///< 입력 RTSP URL
            std::string user_id;
            std::string user_pw;
            int latency_ms          = 200;
            int reconnect_initial_sec = 1;       ///< 첫 reconnect 대기 (초)
            int reconnect_max_sec     = 60;      ///< exponential backoff 상한 (초)
            size_t queue_max_size     = 10;      ///< AVFrame 큐 capacity
            bool enable_raw_passthrough = false; ///< true 면 raw RTP H.264 packet callback 도 받음 (video forward)
        };

        using FrameQueue    = SafeQueue<std::shared_ptr<AVFrame>>;
        using FrameQueuePtr = std::shared_ptr<FrameQueue>;
        using RawPacketCb   = std::function<void(const uint8_t*, size_t)>;

        /**
         * @param cfg   설정 (rtsp_url 필수)
         * @param queue AVFrame 출력 큐. 외부 소유 (보통 IOStreamManager). max_size 도 외부에서 설정 가능.
         */
        GstRtspClient( const Config& cfg, FrameQueuePtr queue ) noexcept;
        ~GstRtspClient();

        /// raw RTP H.264 packet callback 등록 (video forward 용).
        /// enable_raw_passthrough=true 시 의미 있음. Start() 이전에 등록 권장.
        void SetRawPacketCallback( RawPacketCb cb ) noexcept;

        GstRtspClient( const GstRtspClient& )            = delete;
        GstRtspClient& operator=( const GstRtspClient& ) = delete;
        GstRtspClient( GstRtspClient&& )                 = delete;
        GstRtspClient& operator=( GstRtspClient&& )      = delete;

        /** Receiver 시작 + reconnect 워커 스레드 시작. */
        bool Start() noexcept;

        /** Receiver 종료 + reconnect 워커 스레드 종료 (graceful). */
        void Stop() noexcept;

        bool IsRunning() const noexcept { return running_.load(); }

        // 통계
        uint64_t GetFrameCount()     const noexcept;
        uint64_t GetReconnectCount() const noexcept { return reconnect_count_.load(); }
        uint64_t GetEnqueueDropCount() const noexcept { return enqueue_drop_count_.load(); }

    private:
        /** Receiver 의 on_error/on_timeout/on_eos 콜백에서 호출. 비동기 reconnect 트리거. */
        void RequestReconnect() noexcept;

        /** reconnect 워커 스레드 본체. condition_variable 로 wait, 트리거 시 Stop+sleep+Start. */
        void ReconnectWorker();

        /** Receiver 생성 + 콜백 등록 + Start. */
        bool StartReceiver();

        /** Receiver Stop. */
        void StopReceiver();

        Config        cfg_;
        FrameQueuePtr queue_;
        RawPacketCb   raw_cb_;

        std::unique_ptr<GstRtspReceiver> receiver_;
        std::mutex                       receiver_mtx_;     ///< receiver_ 교체 보호

        std::atomic<bool>     running_              { false };
        std::atomic<bool>     shutdown_             { false };
        std::atomic<bool>     reconnect_pending_    { false };
        std::atomic<bool>     eos_reconnect_pending_{ false };  ///< true=EOS cause (immediate restart, no backoff)
        std::atomic<uint64_t> reconnect_count_      { 0 };
        std::atomic<uint64_t> enqueue_drop_count_   { 0 };

        std::mutex              cv_mtx_;
        std::condition_variable cv_;
        std::thread             reconnect_thread_;
        int                     backoff_sec_       = 1;
    };

} // namespace MGEN
