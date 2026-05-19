#include "RtspHandler.h"

#include "MgenLogger.h"

#include <cstring>

#include <gst/gst.h>
#include <gst/app/gstappsrc.h>

namespace MGEN
{
    // ==================================================================================================================
    //   RtspHandler — GStreamer 기반 (Phase 2: receiver + proxy server + ONVIF metadata)
    // ==================================================================================================================

    RtspHandler::RtspHandler( const RtspSetting& setting ) noexcept
        : my_local_ip_                    ( setting.my_local_ip )
        , is_init_done_                   ( false )
        , is_rtsp_proxy_static_with_init_ ( setting.is_rtsp_proxy_static_with_init )
        , proxy_server_port_              ( setting.proxy_server_port )
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

        // Phase 2: output RTSP server 생성 + start.
        //   외부 viewer 가 rtsp://<host>:<port>/cam<id> 로 영상 + ONVIF metadata 수신.
        GstRtspProxyServer::Config srv_cfg;
        srv_cfg.bind_port = this->proxy_server_port_;
        this->proxy_server_ = std::make_unique<GstRtspProxyServer>( srv_cfg );

        if( !this->proxy_server_->Start() ) {
            MLOG_ERROR("RtspHandler::Initialize — GstRtspProxyServer Start 실패");
            this->proxy_server_.reset();
            return false;
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

        this->SetRtspState( RtspState::Run );

        std::lock_guard<std::mutex> lock { this->clients_mtx_ };
        MLOG_INFO("RtspHandler::RunRTSP — %zu camera(s) registered", this->clients_.size() );

        return true;
    }

    bool RtspHandler::StopRTSP( const int /*stop_timeout_seconds*/ )
    {
        if( this->state_ == RtspState::Stop )
            return true;

        // Order:
        //   1. 입력 측 client Stop (RTSP source 수신 중단)
        //   2. 출력 측 proxy_server Stop (graceful 10-step)
        //   3. cam_sinks_ 정리 (payloader 해제 — appsrc 는 factory 가 destroy 시 unref)
        {
            std::lock_guard<std::mutex> lock { this->clients_mtx_ };
            for( auto& [cam_id, client] : this->clients_ ) {
                if( client ) {
                    client->Stop();
                    MLOG_DEBUG("RtspHandler::StopRTSP — CAM[%d] client stopped", cam_id );
                }
            }
            this->clients_.clear();
        }

        if( this->proxy_server_ ) {
            this->proxy_server_->Stop();
            this->proxy_server_.reset();
        }

        {
            std::lock_guard<std::mutex> lock { this->cam_sinks_mtx_ };
            this->cam_sinks_.clear();
        }

        this->SetRtspState( RtspState::Stop );
        MLOG_INFO("RtspHandler::StopRTSP — all clients + proxy_server stopped" );
        return true;
    }

    bool RtspHandler::RunProxiesAfterRuntimeSettingLoad( void )
    {
        // Phase 1: 카메라 등록은 RtspDetectorUnit::Init 가 RegisterClient 으로 수행.
        return true;
    }

    void RtspHandler::OnProxySinkReady( const GstRtspProxyServer::CameraSink& sink )
    {
        std::lock_guard<std::mutex> lock { this->cam_sinks_mtx_ };
        auto& entry = this->cam_sinks_[ sink.cam_id ];
        entry.video_src = sink.video_src;
        if( sink.meta_src ) {
            entry.meta_payloader = std::make_unique<OnvifMetadataPayloader>( sink.meta_src );
        }
        MLOG_INFO("RtspHandler: CAM[%d] proxy sink ready — video=%p meta_payloader=%p",
            sink.cam_id, (void*) sink.video_src, (void*) entry.meta_payloader.get() );
    }

    bool RtspHandler::SendOnvifMetadata( const int cam_id, const std::string& xml_document )
    {
        std::lock_guard<std::mutex> lock { this->cam_sinks_mtx_ };
        auto it = this->cam_sinks_.find( cam_id );
        if( it == this->cam_sinks_.end() || !it->second.meta_payloader ) {
            // 클라이언트 미접속 = 정상. drop 카운트 안 함 (수신자 없는 메타 데이터는 의미 없음).
            return false;
        }
        return it->second.meta_payloader->PushXml( xml_document );
    }

    bool RtspHandler::ForwardVideoRtp( const int cam_id, const uint8_t* data, std::size_t size ) noexcept
    {
        if( !data || size == 0 ) return false;

        GstAppSrc* video_src = nullptr;
        {
            std::lock_guard<std::mutex> lock { this->cam_sinks_mtx_ };
            auto it = this->cam_sinks_.find( cam_id );
            if( it == this->cam_sinks_.end() || !it->second.video_src ) {
                // 클라이언트 미접속 — drop. proxy server 에 viewer 없으면 송출 의미 없음.
                return false;
            }
            video_src = it->second.video_src;
        }

        GstBuffer* buf = gst_buffer_new_allocate( nullptr, size, nullptr );
        if( !buf ) return false;

        GstMapInfo info;
        if( !gst_buffer_map( buf, &info, GST_MAP_WRITE ) ) {
            gst_buffer_unref( buf );
            return false;
        }
        std::memcpy( info.data, data, size );
        gst_buffer_unmap( buf, &info );

        // gst_app_src_push_buffer 가 buffer 소유권 가져감.
        GstFlowReturn fr = gst_app_src_push_buffer( video_src, buf );
        return ( fr == GST_FLOW_OK );
    }

    void RtspHandler::RegisterClient( const int cam_id, std::unique_ptr<GstRtspClient> client ) noexcept
    {
        if( !client ) {
            MLOG_ERROR("RtspHandler::RegisterClient — null client for CAM[%d]", cam_id );
            return;
        }

        {
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

        // Phase 2: 출력 측 mount point 등록.
        //   on_sink callback 은 첫 클라이언트가 mount path 에 접속하는 시점에 호출됨.
        //   따라서 RegisterClient 시점에 즉시 video_src 가 잡히지 않음 — 클라이언트 접속 후 OnProxySinkReady 에서 보관.
        if( this->proxy_server_ ) {
            this->proxy_server_->RegisterCamera( cam_id,
                [this]( const GstRtspProxyServer::CameraSink& s ) { this->OnProxySinkReady( s ); },
                true /* include_metadata */ );
        }
    }

    void RtspHandler::UnregisterClient( const int cam_id ) noexcept
    {
        {
            std::lock_guard<std::mutex> lock { this->clients_mtx_ };

            auto it = this->clients_.find( cam_id );
            if( it == this->clients_.end() )
                return;

            if( it->second ) it->second->Stop();
            this->clients_.erase( it );
        }

        if( this->proxy_server_ ) {
            this->proxy_server_->UnregisterCamera( cam_id );
        }

        {
            std::lock_guard<std::mutex> lock { this->cam_sinks_mtx_ };
            this->cam_sinks_.erase( cam_id );
        }

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
        // GstRtspClient 가 첫 sample 의 caps 에서 width/height 추출하는 API 가 없음 (Phase 1 부터 미구현).
        return std::nullopt;
    }

} // namespace MGEN
