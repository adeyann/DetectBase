/**
 * @file GstRtspProxyServer.h
 * @brief GStreamer 기반 RTSP 서버 — 4cam (각각 video + ONVIF metadata) 출력 + 인증 + graceful shutdown.
 *
 * @details
 * 책임:
 *   - gst-rtsp-server 위에 per-camera mount point 등록 (예: /658, /659, ...)
 *   - 각 mount point 의 factory 가 video(H.264 pre-payloaded RTP) + metadata(ONVIF) 두 stream 노출
 *   - 인증: basic auth (admin/admin + user/123456) + permissions
 *   - Graceful shutdown 10단계
 *   - 외부 코드가 RegisterCamera/UnregisterCamera 로 동적 mount 관리
 */

#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <gst/gst.h>
#include <gst/rtsp-server/rtsp-server.h>
#include <gst/app/gstappsrc.h>

namespace MGEN
{
    class GstRtspProxyServer
    {
    public:
        struct Config
        {
            std::string bind_port = "555";     ///< RTSP listen port (default 555, ServerSetting.RtspPort 로 override)
            // 인증 — happytimesoft 호환 default
            std::string admin_user = "admin";
            std::string admin_pw   = "admin";
            std::string user_user  = "user";
            std::string user_pw    = "123456";
            bool        require_auth = true;   ///< false 면 익명 접근 허용 (PoC 검증용)
        };

        /**
         * @brief 클라이언트 연결 시 호출되는 콜백 — factory 가 새 media 만들 때 appsrc(video/metadata) 핸들 전달.
         *        외부 코드는 이 핸들에 H.264 RTP 또는 ONVIF metadata RTP 를 push.
         */
        struct CameraSink
        {
            int        cam_id     = 0;
            GstAppSrc* video_src  = nullptr;   ///< name="pay0" — H.264 pre-payloaded RTP
            GstAppSrc* meta_src   = nullptr;   ///< name="pay1" — ONVIF metadata RTP (선택)
        };

        using SinkCallback = std::function<void(const CameraSink&)>;

        explicit GstRtspProxyServer( const Config& cfg ) noexcept;
        ~GstRtspProxyServer();

        GstRtspProxyServer( const GstRtspProxyServer& )            = delete;
        GstRtspProxyServer& operator=( const GstRtspProxyServer& ) = delete;

        /**
         * @brief 서버 시작 + main loop 스레드 실행.
         */
        bool Start() noexcept;

        /**
         * @brief 카메라 등록 — mount point 생성.
         *
         * @param cam_id      카메라 ID (mount path = "/<id>")
         * @param on_sink     클라이언트 연결 시 appsrc 핸들 전달 콜백 (외부에서 buffer push)
         * @param include_metadata true 면 ONVIF metadata stream 도 등록 (pay1)
         * @return true 성공
         */
        bool RegisterCamera( int cam_id, SinkCallback on_sink, bool include_metadata = true ) noexcept;

        /**
         * @brief 카메라 등록 해제 — mount point 제거.
         */
        bool UnregisterCamera( int cam_id ) noexcept;

        /**
         * @brief Graceful shutdown 10단계.
         */
        void Stop() noexcept;

        bool IsRunning() const noexcept { return running_.load(); }

        // 통계
        uint64_t GetCameraCount() const noexcept;

    private:
        struct FactoryContext
        {
            int          cam_id;
            SinkCallback on_sink;
            bool         include_metadata;
        };

        void SetupAuthentication() noexcept;
        void MainLoopThreadFunc();

        // factory 의 "media-configure" 시그널 핸들러
        static void OnMediaConfigure( GstRTSPMediaFactory* factory,
                                      GstRTSPMedia* media, gpointer user_data );

        Config              cfg_;
        GstRTSPServer*      server_       = nullptr;
        GstRTSPMountPoints* mounts_       = nullptr;
        GstRTSPAuth*        auth_         = nullptr;
        GstRTSPToken*       admin_token_  = nullptr;
        GstRTSPToken*       user_token_   = nullptr;
        GMainLoop*          loop_         = nullptr;
        std::thread         loop_thread_;
        guint               server_source_id_ = 0;

        // attach 동기화 — Start() 가 loop+attach 완료까지 wait (race 제거)
        std::mutex              attach_mtx_;
        std::condition_variable attach_cv_;
        bool                    attach_done_ = false;

        // FactoryContext lifetime: 등록 시 heap allocate, factory destroyed 시 GDestroyNotify 로 해제
        std::map<int, std::string>                       mount_paths_;     ///< cam_id → "/<id>"
        std::map<int, std::unique_ptr<FactoryContext>>   factory_ctxs_;
        mutable std::mutex                               cameras_mtx_;

        std::atomic<bool> running_  { false };
        std::atomic<bool> shutdown_ { false };
    };

} // namespace MGEN
