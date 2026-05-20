/**
 * @file GstRtspClient.cpp
 * @brief 통합 RTSP client — Receiver + auto reconnect + AVFrame queue.
 */

#include "GstRtspClient.h"

#include "MgenLogger.h"
#include "MetricsRegistry.h"

extern "C" {
#include <libavutil/frame.h>
}

#include <chrono>
#include <mutex>

namespace MGEN
{
    namespace
    {
        constexpr const char* METRIC_RECONNECT_TOTAL    = "detectbase_gst_rtsp_client_reconnect_total";
        constexpr const char* METRIC_ENQUEUE_DROP_TOTAL = "detectbase_gst_rtsp_client_enqueue_drop_total";

        const std::map<std::string, std::string> NO_LABELS;

        // TSan: 여러 GstRtspClient ctor 가 동시에 register 시 race. call_once 로 한 번만 실행 보장.
        std::once_flag g_metrics_once;

        void RegisterMetricsOnce() noexcept
        {
            std::call_once( g_metrics_once, []{
                auto& reg = MetricsRegistry::Instance();
                reg.RegisterCounter( METRIC_RECONNECT_TOTAL,    "GstRtspClient successful reconnect attempts" );
                reg.RegisterCounter( METRIC_ENQUEUE_DROP_TOTAL, "GstRtspClient frame drops (queue full)" );
            } );
        }
    } // anonymous

    GstRtspClient::GstRtspClient( const Config& cfg, FrameQueuePtr queue ) noexcept
        : cfg_  ( cfg )
        , queue_( std::move( queue ) )
    {
        RegisterMetricsOnce();
        if( queue_ ) {
            queue_->SetMaxSize( cfg_.queue_max_size );
        }
        backoff_sec_ = cfg_.reconnect_initial_sec;
        MLOG_INFO( "GstRtspClient[%d] 생성 — url=%s", cfg_.cam_id, cfg_.rtsp_url.c_str() );
    }

    GstRtspClient::~GstRtspClient()
    {
        Stop();
    }

    bool GstRtspClient::Start() noexcept
    {
        if( running_.exchange( true ) ) {
            MLOG_WARN( "GstRtspClient[%d]::Start 이미 실행 중", cfg_.cam_id );
            return false;
        }
        shutdown_.store( false );

        reconnect_thread_ = std::thread( [this]{ ReconnectWorker(); } );

        if( !StartReceiver() ) {
            shutdown_.store( true );
            cv_.notify_all();
            if( reconnect_thread_.joinable() ) reconnect_thread_.join();
            running_.store( false );
            return false;
        }
        return true;
    }

    void GstRtspClient::Stop() noexcept
    {
        if( !running_.exchange( false ) ) return;

        shutdown_.store( true );
        cv_.notify_all();

        if( reconnect_thread_.joinable() ) {
            reconnect_thread_.join();
        }
        StopReceiver();

        MLOG_INFO( "GstRtspClient[%d] Stop — reconnect=%lu enqueue_drop=%lu",
            cfg_.cam_id,
            (unsigned long) reconnect_count_.load(),
            (unsigned long) enqueue_drop_count_.load() );
    }

    void GstRtspClient::SetRawPacketCallback( RawPacketCb cb ) noexcept
    {
        raw_cb_ = std::move( cb );
    }

    bool GstRtspClient::StartReceiver()
    {
        std::lock_guard<std::mutex> lk( receiver_mtx_ );

        GstRtspReceiver::Config rcfg;
        rcfg.url           = cfg_.rtsp_url;
        rcfg.user_id       = cfg_.user_id;
        rcfg.user_pw       = cfg_.user_pw;
        rcfg.latency_ms    = cfg_.latency_ms;
        rcfg.enable_raw_passthrough = cfg_.enable_raw_passthrough;

        // 진단 (debug/gst-rtsp-stale-trace 2026-05-20) — cam_id 전달
        rcfg.cam_id = cfg_.cam_id;

        // bus 스레드 콜백 → 비동기 reconnect 요청 (deadlock 회피)
        rcfg.on_error   = [this]{
            MLOG_WARN( "GstRtspClient[%d] on_error trigger → RequestReconnect", cfg_.cam_id );
            RequestReconnect();
        };
        rcfg.on_timeout = [this]{
            MLOG_WARN( "GstRtspClient[%d] on_timeout trigger → RequestReconnect", cfg_.cam_id );
            RequestReconnect();
        };
        // BISECTION 복구: EOS reconnect 재활성
        rcfg.on_eos     = [this]{
            MLOG_WARN( "GstRtspClient[%d] on_eos trigger → RequestReconnect (eos_pending=true)", cfg_.cam_id );
            eos_reconnect_pending_.store( true );
            RequestReconnect();
        };

        auto frame_cb = [this]( std::shared_ptr<AVFrame> frame ) {
            if( !frame || !queue_ ) return;

            // Frame interval 실측 (drop 여부 무관 — 매 호출 = camera 자체 FPS).
            const int64_t now_cb_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch() ).count();
            const int64_t last_cb   = last_cb_ns_.exchange( now_cb_ns );
            if( last_cb != 0 ) {
                const int64_t interval = now_cb_ns - last_cb;
                if( interval > 0 && interval < 1'000'000'000LL ) {  // 1 sec 미만만 (reconnect 등 outlier 배제)
                    frame_interval_sum_ns_.fetch_add( static_cast<uint64_t>( interval ) );
                    frame_interval_count_.fetch_add( 1 );
                }
            }

            // backpressure 정책: 큐 size>0 drop 만 유지 (decoded I420 frame 메모리 폭탄 방지, 큐 평균 0~1 frame).
            //   이전: interval (1000/fps_limit) ms drop 도 적용 — "NPU 가 30 FPS 천장" 가정의 over-cap.
            //   A-plan multi-handler 후 NPU 천장 ~140 FPS 라 interval 정책은 무효한 가정. camera jitter
            //   + interval floating 정밀도로 매 2번째 frame drop → dfps 70 (천장 120 의 58%) 으로 묶임.
            //   제거 결과: camera 30 FPS 모든 frame 통과, dfps 이론 천장 120 도달 (cam thread cycle 11ms < 33ms).
            if( queue_->size() > 0 ) {
                enqueue_drop_count_.fetch_add( 1 );
                MetricsRegistry::Instance().IncrementCounter( METRIC_ENQUEUE_DROP_TOTAL, NO_LABELS, 1.0 );
                return;
            }
            queue_->enqueue_move( std::move( frame ) );
        };

        receiver_ = std::make_unique<GstRtspReceiver>( rcfg, frame_cb );
        if( cfg_.enable_raw_passthrough && raw_cb_ ) {
            receiver_->SetRawPacketCallback( raw_cb_ );
        }
        if( !receiver_->Start() ) {
            receiver_.reset();
            return false;
        }
        return true;
    }

    void GstRtspClient::StopReceiver()
    {
        std::unique_ptr<GstRtspReceiver> rx;
        {
            std::lock_guard<std::mutex> lk( receiver_mtx_ );
            rx = std::move( receiver_ );
        }
        if( rx ) {
            rx->Stop();
            rx.reset();
        }
    }

    void GstRtspClient::RequestReconnect() noexcept
    {
        if( shutdown_.load() ) {
            MLOG_DEBUG( "GstRtspClient[%d] RequestReconnect ignored (shutdown)", cfg_.cam_id );
            return;
        }
        bool expected = false;
        if( reconnect_pending_.compare_exchange_strong( expected, true ) ) {
            MLOG_INFO( "GstRtspClient[%d] reconnect_pending set → notify worker (eos_pending=%d)",
                cfg_.cam_id, eos_reconnect_pending_.load() ? 1 : 0 );
            cv_.notify_one();
        } else {
            MLOG_INFO( "GstRtspClient[%d] RequestReconnect ignored (already pending, eos_pending=%d)",
                cfg_.cam_id, eos_reconnect_pending_.load() ? 1 : 0 );
        }
    }

    void GstRtspClient::ReconnectWorker()
    {
        MLOG_INFO( "GstRtspClient[%d] reconnect worker 시작", cfg_.cam_id );

        while( !shutdown_.load() )
        {
            {
                std::unique_lock<std::mutex> lk( cv_mtx_ );
                cv_.wait( lk, [this]{ return reconnect_pending_.load() || shutdown_.load(); } );
            }
            if( shutdown_.load() ) break;

            const bool is_eos_at_wake = eos_reconnect_pending_.load();
            MLOG_INFO( "GstRtspClient[%d] reconnect worker wake — eos_pending=%d", cfg_.cam_id, is_eos_at_wake ? 1 : 0 );
            reconnect_pending_.store( false );

            // EOS 인 경우 — 카메라 mp4 5분 cycle 등 정상 stream 종료.
            //   → in-place reset 시도 (rtspsrc 만 NULL→PLAYING, mppvideodec 보존)
            //   → mpp internal hardware DMA buffer leak (~12MB/reconn) 회피
            // error/timeout 인 경우 — full restart + exponential backoff.
            const bool is_eos = eos_reconnect_pending_.exchange( false );

            if( is_eos ) {
                // In-place reset 시도 (receiver_ 보존)
                bool inplace_ok = false;
                {
                    std::lock_guard<std::mutex> lk( receiver_mtx_ );
                    if( receiver_ ) {
                        inplace_ok = receiver_->ResetSourceOnly();
                    }
                }
                if( inplace_ok ) {
                    reconnect_count_.fetch_add( 1 );
                    MetricsRegistry::Instance().IncrementCounter( METRIC_RECONNECT_TOTAL, NO_LABELS, 1.0 );
                    MLOG_INFO( "GstRtspClient[%d] EOS in-place reset OK — mppvideodec 보존", cfg_.cam_id );
                    backoff_sec_ = cfg_.reconnect_initial_sec;
                    continue;  // 다음 cycle 으로 (다시 wait)
                }
                MLOG_WARN( "GstRtspClient[%d] EOS in-place reset 실패 — full restart 로 fallback", cfg_.cam_id );
                // fallback: 아래 full restart path 로 (wait_sec=0)
            }

            const int wait_sec = is_eos ? 0 : backoff_sec_;
            if( !is_eos ) {
                MLOG_WARN( "GstRtspClient[%d] reconnect 트리거 — %ds 후 재시도", cfg_.cam_id, wait_sec );
            }

            // 분할 sleep 으로 shutdown 빠르게 대응 (wait_sec=0 이면 즉시 진행)
            for( int i = 0; i < wait_sec && !shutdown_.load(); ++i ) {
                std::this_thread::sleep_for( std::chrono::seconds( 1 ) );
            }
            if( shutdown_.load() ) break;

            StopReceiver();
            if( StartReceiver() ) {
                reconnect_count_.fetch_add( 1 );
                MetricsRegistry::Instance().IncrementCounter( METRIC_RECONNECT_TOTAL, NO_LABELS, 1.0 );
                MLOG_INFO( "GstRtspClient[%d] reconnect 성공 — backoff reset", cfg_.cam_id );
                backoff_sec_ = cfg_.reconnect_initial_sec; // 성공 시 backoff 초기화
            } else {
                // 실패 시 backoff 2배 증가 (상한)
                backoff_sec_ = std::min( backoff_sec_ * 2, cfg_.reconnect_max_sec );
                MLOG_ERROR( "GstRtspClient[%d] reconnect 실패 — backoff 증가 (%ds)", cfg_.cam_id, backoff_sec_ );
                // 즉시 다시 트리거 (지속 재시도)
                reconnect_pending_.store( true );
                cv_.notify_one();
            }
        }
        MLOG_INFO( "GstRtspClient[%d] reconnect worker 종료", cfg_.cam_id );
    }

    uint64_t GstRtspClient::GetFrameCount() const noexcept
    {
        std::lock_guard<std::mutex> lk( const_cast<std::mutex&>( receiver_mtx_ ) );
        return receiver_ ? receiver_->GetFrameCount() : 0;
    }

    uint32_t GstRtspClient::GetAppSinkQueuedBuffers() const noexcept
    {
        std::lock_guard<std::mutex> lk( const_cast<std::mutex&>( receiver_mtx_ ) );
        return receiver_ ? receiver_->GetAppSinkQueuedBuffers() : 0;
    }

    uint64_t GstRtspClient::GetResetSourceCount() const noexcept
    {
        std::lock_guard<std::mutex> lk( const_cast<std::mutex&>( receiver_mtx_ ) );
        return receiver_ ? receiver_->GetResetSourceCount() : 0;
    }

    int64_t GstRtspClient::GetLastFrameNs() const noexcept
    {
        std::lock_guard<std::mutex> lk( const_cast<std::mutex&>( receiver_mtx_ ) );
        return receiver_ ? receiver_->GetLastFrameNs() : 0;
    }

} // namespace MGEN
