/**
 * @file GstRtspProxyServer.cpp
 * @brief GStreamer RTSP server 구현 — 인증, shutdown 10단계, per-cam mount.
 */

#include "GstRtspProxyServer.h"

#include "MgenLogger.h"

namespace MGEN
{
    GstRtspProxyServer::GstRtspProxyServer( const Config& cfg ) noexcept
        : cfg_( cfg )
    {
        MLOG_INFO( "GstRtspProxyServer 생성 — port=%s require_auth=%d",
            cfg_.bind_port.c_str(), (int) cfg_.require_auth );
    }

    GstRtspProxyServer::~GstRtspProxyServer()
    {
        Stop();
    }

    bool GstRtspProxyServer::Start() noexcept
    {
        if( running_.exchange( true ) ) {
            MLOG_WARN( "GstRtspProxyServer::Start 이미 실행 중" );
            return false;
        }
        shutdown_.store( false );

        server_ = gst_rtsp_server_new();
        if( !server_ ) {
            MLOG_ERROR( "gst_rtsp_server_new 실패" );
            running_.store( false );
            return false;
        }
        gst_rtsp_server_set_service( server_, cfg_.bind_port.c_str() );

        mounts_ = gst_rtsp_server_get_mount_points( server_ );

        if( cfg_.require_auth ) {
            SetupAuthentication();
        }

        // GMainLoop 스레드 시작 (attach 는 main context 안에서 해야 하므로 thread 안에서)
        {
            std::lock_guard<std::mutex> lk( attach_mtx_ );
            attach_done_ = false;
        }
        loop_thread_ = std::thread( [this] { MainLoopThreadFunc(); } );

        // attach 완료까지 wait — race 제거 (50ms sleep 대체).
        // 5초 timeout — 그 안 안되면 init 실패로 간주.
        {
            std::unique_lock<std::mutex> lk( attach_mtx_ );
            if( !attach_cv_.wait_for( lk, std::chrono::seconds( 5 ),
                    [this]{ return attach_done_; } ) ) {
                MLOG_ERROR( "GstRtspProxyServer Start — attach timeout (5s)" );
                running_.store( false );
                return false;
            }
        }

        MLOG_INFO( "GstRtspProxyServer Start OK — rtsp://0.0.0.0:%s/", cfg_.bind_port.c_str() );
        return true;
    }

    void GstRtspProxyServer::MainLoopThreadFunc()
    {
        loop_ = g_main_loop_new( nullptr, FALSE );
        server_source_id_ = gst_rtsp_server_attach( server_, nullptr );

        // attach 완료 신호 — Start() 가 wait 중
        {
            std::lock_guard<std::mutex> lk( attach_mtx_ );
            attach_done_ = true;
        }
        attach_cv_.notify_all();

        g_main_loop_run( loop_ );
        g_main_loop_unref( loop_ );
        loop_ = nullptr;
    }

    void GstRtspProxyServer::SetupAuthentication() noexcept
    {
        auth_ = gst_rtsp_auth_new();

        // admin (full access)
        admin_token_ = gst_rtsp_token_new(
            GST_RTSP_TOKEN_MEDIA_FACTORY_ROLE, G_TYPE_STRING, "admin", nullptr );
        gchar* basic_admin = gst_rtsp_auth_make_basic( cfg_.admin_user.c_str(), cfg_.admin_pw.c_str() );
        gst_rtsp_auth_add_basic( auth_, basic_admin, admin_token_ );
        g_free( basic_admin );

        // user (view only)
        user_token_ = gst_rtsp_token_new(
            GST_RTSP_TOKEN_MEDIA_FACTORY_ROLE, G_TYPE_STRING, "user", nullptr );
        gchar* basic_user = gst_rtsp_auth_make_basic( cfg_.user_user.c_str(), cfg_.user_pw.c_str() );
        gst_rtsp_auth_add_basic( auth_, basic_user, user_token_ );
        g_free( basic_user );

        gst_rtsp_server_set_auth( server_, auth_ );

        MLOG_INFO( "GstRtspProxyServer 인증 설정 — admin/%s + user/%s",
            cfg_.admin_pw.c_str(), cfg_.user_pw.c_str() );
    }

    void GstRtspProxyServer::OnMediaConfigure( GstRTSPMediaFactory* /*factory*/,
                                               GstRTSPMedia* media, gpointer user_data )
    {
        auto* ctx = static_cast<FactoryContext*>( user_data );
        if( !ctx || !ctx->on_sink ) return;

        CameraSink sink;
        sink.cam_id = ctx->cam_id;

        GstElement* element = gst_rtsp_media_get_element( media );

        // pay0 = video appsrc (RTP packet).
        GstElement* video = gst_bin_get_by_name_recurse_up( GST_BIN( element ), "pay0" );
        if( video ) {
            sink.video_src = GST_APP_SRC( video );
            g_object_set( video, "is-live", TRUE, "format", GST_FORMAT_TIME,
                "block", FALSE, "max-bytes", (guint64) ( 16 * 1024 * 1024 ), nullptr );
            // factory 내부에서 ref count 가 유지되므로 외부 ref unref 안 함
        }

        // pay1 = metadata appsrc (옵션)
        if( ctx->include_metadata ) {
            GstElement* meta = gst_bin_get_by_name_recurse_up( GST_BIN( element ), "pay1" );
            if( meta ) {
                sink.meta_src = GST_APP_SRC( meta );
                g_object_set( meta, "is-live", TRUE, "format", GST_FORMAT_TIME,
                    "block", FALSE, "max-bytes", (guint64) ( 4 * 1024 * 1024 ), nullptr );
            }
        }

        gst_object_unref( element );

        MLOG_INFO( "GstRtspProxyServer cam=%d media-configure: video_src=%p meta_src=%p",
            ctx->cam_id, (void*) sink.video_src, (void*) sink.meta_src );

        ctx->on_sink( sink );
    }

    bool GstRtspProxyServer::RegisterCamera( int cam_id, SinkCallback on_sink, bool include_metadata ) noexcept
    {
        if( !running_.load() ) {
            MLOG_ERROR( "RegisterCamera[%d] — server 미시작", cam_id );
            return false;
        }

        std::lock_guard<std::mutex> lk( cameras_mtx_ );
        if( factory_ctxs_.count( cam_id ) ) {
            MLOG_WARN( "RegisterCamera[%d] — 이미 등록됨", cam_id );
            return false;
        }

        const std::string path = "/" + std::to_string( cam_id );

        GstRTSPMediaFactory* factory = gst_rtsp_media_factory_new();
        gst_rtsp_media_factory_set_shared( factory, TRUE );

        // launch line:
        //   - video(pay0): receiver 가 rtph264pay 후의 RTP packet 을 그대로 push.
        //                   gst-rtsp-server 는 name=pay<N> 을 stream <N> 으로 인식.
        //   - metadata(pay1): OnvifMetadataPayloader 가 RTP 헤더 까지 직접 작성하여 push.
        std::string launch;
        launch += "( appsrc name=pay0 is-live=true "
                  "caps=\"application/x-rtp,media=video,"
                  "encoding-name=H264,clock-rate=90000,payload=96\" ";
        if( include_metadata ) {
            launch += "appsrc name=pay1 is-live=true "
                      "caps=\"application/x-rtp,media=application,"
                      "encoding-name=VND.ONVIF.METADATA,clock-rate=90000,payload=97\" ";
        }
        launch += ")";
        gst_rtsp_media_factory_set_launch( factory, launch.c_str() );

        // permissions (인증 시)
        if( cfg_.require_auth ) {
            GstRTSPPermissions* perms = gst_rtsp_permissions_new();
            gst_rtsp_permissions_add_role( perms, "admin",
                GST_RTSP_PERM_MEDIA_FACTORY_ACCESS,    G_TYPE_BOOLEAN, TRUE,
                GST_RTSP_PERM_MEDIA_FACTORY_CONSTRUCT, G_TYPE_BOOLEAN, TRUE, nullptr );
            gst_rtsp_permissions_add_role( perms, "user",
                GST_RTSP_PERM_MEDIA_FACTORY_ACCESS,    G_TYPE_BOOLEAN, TRUE,
                GST_RTSP_PERM_MEDIA_FACTORY_CONSTRUCT, G_TYPE_BOOLEAN, TRUE, nullptr );
            gst_rtsp_media_factory_set_permissions( factory, perms );
            gst_rtsp_permissions_unref( perms );
        }

        // FactoryContext 등록 (lifetime: factory 가 free 될 때까지)
        auto ctx = std::make_unique<FactoryContext>();
        ctx->cam_id           = cam_id;
        ctx->on_sink          = std::move( on_sink );
        ctx->include_metadata = include_metadata;
        FactoryContext* raw_ctx = ctx.get();

        g_signal_connect( factory, "media-configure",
            G_CALLBACK( &GstRtspProxyServer::OnMediaConfigure ), raw_ctx );

        gst_rtsp_mount_points_add_factory( mounts_, path.c_str(), factory );
        // factory 의 ref 는 mount points 가 가져감

        mount_paths_[ cam_id ]  = path;
        factory_ctxs_[ cam_id ] = std::move( ctx );

        MLOG_INFO( "RegisterCamera[%d] OK — path=%s metadata=%d", cam_id, path.c_str(), (int) include_metadata );
        return true;
    }

    bool GstRtspProxyServer::UnregisterCamera( int cam_id ) noexcept
    {
        std::lock_guard<std::mutex> lk( cameras_mtx_ );
        auto it = mount_paths_.find( cam_id );
        if( it == mount_paths_.end() ) return false;

        gst_rtsp_mount_points_remove_factory( mounts_, it->second.c_str() );
        mount_paths_.erase( it );
        factory_ctxs_.erase( cam_id );

        MLOG_INFO( "UnregisterCamera[%d] OK", cam_id );
        return true;
    }

    uint64_t GstRtspProxyServer::GetCameraCount() const noexcept
    {
        std::lock_guard<std::mutex> lk( cameras_mtx_ );
        return mount_paths_.size();
    }

    /**
     * Graceful shutdown 10단계.
     * !!! DO NOT REORDER !!!
     */
    void GstRtspProxyServer::Stop() noexcept
    {
        if( !running_.exchange( false ) ) return;
        shutdown_.store( true );

        // 1. 새 클라이언트 거부 — server attach 해제
        if( server_source_id_ != 0 && loop_ ) {
            GMainContext* ctx = g_main_loop_get_context( loop_ );
            GSource* src = g_main_context_find_source_by_id( ctx, server_source_id_ );
            if( src ) g_source_destroy( src );
            server_source_id_ = 0;
        }

        // 2. 현재 연결된 클라이언트 모두 disconnect
        if( server_ ) {
            GstRTSPSessionPool* pool = gst_rtsp_server_get_session_pool( server_ );
            if( pool ) {
                gst_rtsp_session_pool_cleanup( pool );
                g_object_unref( pool );
            }
        }

        // 3. mount points 정리
        {
            std::lock_guard<std::mutex> lk( cameras_mtx_ );
            if( mounts_ ) {
                for( const auto& [ id, path ] : mount_paths_ ) {
                    gst_rtsp_mount_points_remove_factory( mounts_, path.c_str() );
                }
            }
            mount_paths_.clear();
            factory_ctxs_.clear();
        }

        // 4. mount points unref
        if( mounts_ ) { g_object_unref( mounts_ ); mounts_ = nullptr; }

        // 5. auth unref
        if( auth_ )        { g_object_unref( auth_ );        auth_        = nullptr; }
        if( admin_token_ ) { gst_rtsp_token_unref( admin_token_ ); admin_token_ = nullptr; }
        if( user_token_ )  { gst_rtsp_token_unref( user_token_ );  user_token_  = nullptr; }

        // 6. GMainLoop quit
        if( loop_ ) {
            g_main_loop_quit( loop_ );
        }

        // 7. main loop thread join
        if( loop_thread_.joinable() ) {
            loop_thread_.join();
        }

        // 8. server unref
        if( server_ ) { g_object_unref( server_ ); server_ = nullptr; }

        // 9. (확장 영역) 추가 자원 해제
        // 10. log
        MLOG_INFO( "GstRtspProxyServer Stop OK (shutdown 10단계 완료)" );
    }

} // namespace MGEN
