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

namespace MGEN
{
    namespace
    {
        constexpr const char* METRIC_RECONNECT_TOTAL    = "detectbase_gst_rtsp_client_reconnect_total";
        constexpr const char* METRIC_ENQUEUE_DROP_TOTAL = "detectbase_gst_rtsp_client_enqueue_drop_total";

        const std::map<std::string, std::string> NO_LABELS;

        bool g_metrics_registered = false;

        void RegisterMetricsOnce() noexcept
        {
            if( g_metrics_registered ) return;
            auto& reg = MetricsRegistry::Instance();
            reg.RegisterCounter( METRIC_RECONNECT_TOTAL,    "GstRtspClient successful reconnect attempts" );
            reg.RegisterCounter( METRIC_ENQUEUE_DROP_TOTAL, "GstRtspClient frame drops (queue full)" );
            g_metrics_registered = true;
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

        // bus 스레드 콜백 → 비동기 reconnect 요청 (deadlock 회피)
        rcfg.on_error   = [this]{ RequestReconnect(); };
        rcfg.on_timeout = [this]{ RequestReconnect(); };
        // BISECTION 복구: EOS reconnect 재활성
        rcfg.on_eos     = [this]{ eos_reconnect_pending_.store( true ); RequestReconnect(); };

        auto frame_cb = [this]( std::shared_ptr<AVFrame> frame ) {
            if( !frame || !queue_ ) return;
            // SafeQueue 의 max_size + drop_oldest 정책 활용 (자체 drop_count 없으니 size 사전 체크)
            const size_t before = queue_->size();
            queue_->enqueue_move( std::move( frame ) );
            // enqueue 후에도 size 변동 없으면 drop 발생한 것 (capacity 가득)
            if( before >= cfg_.queue_max_size ) {
                enqueue_drop_count_.fetch_add( 1 );
                MetricsRegistry::Instance().IncrementCounter( METRIC_ENQUEUE_DROP_TOTAL, NO_LABELS, 1.0 );
            }
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
        if( shutdown_.load() ) return;
        bool expected = false;
        if( reconnect_pending_.compare_exchange_strong( expected, true ) ) {
            cv_.notify_one();
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

            reconnect_pending_.store( false );

            // Phase 1.1 (Q18 fix, 2026-05-16): EOS / error / timeout 모두 full restart 로 통합.
            // 이전엔 EOS 시 ResetSourceOnly (TeardownPipeline + BuildPipeline) 호출했으나,
            //   (a) Phase 1 = avdec_h264 (mppvideodec 없음) — in-place reset motivation 없음
            //   (b) ResetSourceOnly 가 호출될 때마다 rtspsrc UDP RTP/RTCP socket 누수
            //   (c) 1h 측정 결과 ~2 socket/reconnect leak
            // full restart 도 같은 TeardownPipeline 호출하므로 leak path 자체 동일하지만,
            // wait_sec 만큼 reconnect 빈도 줄여서 절대 leak rate 감소. + rtspsrc 의
            // protocols=tcp 와 결합되어 UDP socket 자체 0.
            const bool is_eos = eos_reconnect_pending_.exchange( false );
            const int wait_sec = is_eos ? 0 : backoff_sec_;
            if( is_eos ) {
                MLOG_INFO( "GstRtspClient[%d] EOS — full restart (in-place reset deprecated)", cfg_.cam_id );
            } else {
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

} // namespace MGEN
