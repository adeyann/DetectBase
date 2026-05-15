#include "RtspHandler.h"

#include "MgenLogger.h"

#include <gst/gst.h>

namespace MGEN
{
    // ==================================================================================================================
    //   RtspHandler — GStreamer 기반 (Phase 1)
    // ==================================================================================================================

    RtspHandler::RtspHandler( const RtspSetting setting ) noexcept
        : my_local_ip_                    ( setting.my_local_ip )
        , is_init_done_                   ( false )
        , is_rtsp_proxy_static_with_init_ ( setting.is_rtsp_proxy_static_with_init )
        , state_                          ( RtspState::Create )
    {
        //
    }

    RtspHandler::~RtspHandler()
    {
        this->StopRTSP();
    }

    bool RtspHandler::Initialize( void )
    {
        if( this->is_init_done_ )
            return true;

        // gst_init 은 idempotent — 여러 번 호출해도 안전 (Main 의 다른 모듈도 호출 가능)
        if( gst_is_initialized() == FALSE ) {
            gst_init( nullptr, nullptr );
            MLOG_INFO("RtspHandler: gst_init done (GStreamer %s)", gst_version_string() );
        }
        else {
            MLOG_DEBUG("RtspHandler: GStreamer already initialized");
        }

        this->SetRtspState( RtspState::Ready );
        this->is_init_done_ = true;
        return true;
    }

    bool RtspHandler::IsProxySettingStaticUseInitData( void ) const
    {
        return this->is_rtsp_proxy_static_with_init_;
    }

    bool RtspHandler::RunRTSP( void )
    {
        if( this->state_ == RtspState::Run ) {
            MLOG_DEBUG("RtspHandler::RunRTSP — already running");
            return true;
        }

        // GStreamer 환경에서는 카메라별 GstRtspClient 가 자체 mainloop thread 를 보유.
        // RtspDetectorUnit::Init 시 RegisterClient 가 이미 호출되었거나 (정적 init data),
        // 또는 RunProxiesAfterRuntimeSettingLoad 에서 등록됨.

        this->SetRtspState( RtspState::Run );

        std::lock_guard<std::mutex> lock { this->clients_mtx_ };
        MLOG_INFO("RtspHandler::RunRTSP — %zu camera(s) registered", this->clients_.size() );

        return true;
    }

    bool RtspHandler::StopRTSP( const int /*stop_timeout_seconds*/ )
    {
        if( this->state_ == RtspState::Stop )
            return true;

        std::lock_guard<std::mutex> lock { this->clients_mtx_ };
        for( auto& [cam_id, client] : this->clients_ ) {
            if( client ) {
                client->Stop();
                MLOG_DEBUG("RtspHandler::StopRTSP — CAM[%d] stopped", cam_id );
            }
        }
        this->clients_.clear();

        this->SetRtspState( RtspState::Stop );
        MLOG_INFO("RtspHandler::StopRTSP — all clients stopped" );
        return true;
    }

    bool RtspHandler::RunProxiesAfterRuntimeSettingLoad( void )
    {
        // Phase 1: 카메라 등록은 RtspDetectorUnit::Init 가 RegisterClient 으로 수행.
        // runtime setting reload 후 추가 등록이 필요하면 SettingMonitor 가 직접 처리.
        return true;
    }

    void RtspHandler::RegisterClient( const int cam_id, std::unique_ptr<GstRtspClient> client ) noexcept
    {
        if( !client ) {
            MLOG_ERROR("RtspHandler::RegisterClient — null client for CAM[%d]", cam_id );
            return;
        }

        std::lock_guard<std::mutex> lock { this->clients_mtx_ };

        // 기존 등록이 있으면 stop + 교체
        auto it = this->clients_.find( cam_id );
        if( it != this->clients_.end() ) {
            if( it->second ) it->second->Stop();
            this->clients_.erase( it );
            MLOG_INFO("RtspHandler::RegisterClient — CAM[%d] replaced", cam_id );
        }

        this->clients_[cam_id] = std::move( client );
        MLOG_INFO("RtspHandler::RegisterClient — CAM[%d] registered (total %zu)", cam_id, this->clients_.size() );
    }

    void RtspHandler::UnregisterClient( const int cam_id ) noexcept
    {
        std::lock_guard<std::mutex> lock { this->clients_mtx_ };

        auto it = this->clients_.find( cam_id );
        if( it == this->clients_.end() )
            return;

        if( it->second ) it->second->Stop();
        this->clients_.erase( it );
        MLOG_INFO("RtspHandler::UnregisterClient — CAM[%d] removed", cam_id );
    }

    void RtspHandler::SetRtspState( const RtspState state ) noexcept
    {
        this->state_ = state;
    }

    std::set<int> RtspHandler::GetProxyIDs( void ) const
    {
        std::set<int> id_set;

        std::lock_guard<std::mutex> lock { this->clients_mtx_ };
        for( const auto& [cam_id, client] : this->clients_ ) {
            (void)client;
            id_set.insert( cam_id );
        }
        return id_set;
    }

    GstRtspClient* RtspHandler::GetProxyPtr( const int cam_id ) const
    {
        std::lock_guard<std::mutex> lock { this->clients_mtx_ };

        auto it = this->clients_.find( cam_id );
        if( it == this->clients_.end() )
            return nullptr;

        return it->second.get();
    }

    std::optional<ProxyVideoInfo> RtspHandler::GetProxyInfo( const int /*cam_id*/ ) const
    {
        // Phase 1: GstRtspClient 가 첫 sample 의 caps 에서 width/height 추출하는 API 가 없음.
        // Phase 2 에서 GstRtspClient 에 GetVideoInfo() 메소드 추가 후 여기서 forward.
        // 현재는 unset — RtspDetectorUnit 이 첫 AVFrame 의 width/height 를 직접 사용해야 함.
        return std::nullopt;
    }

} // namespace MGEN
