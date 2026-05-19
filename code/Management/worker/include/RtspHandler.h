#pragma once

#include "GstRtspClient.h"           // Protocol/RTSP_GST
#include "GstRtspProxyServer.h"      // Phase 2 — output RTSP server
#include "OnvifMetadataPayloader.h"  // Phase 2 — ONVIF metadata RTP push

#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>

namespace MGEN
{
    /// happytimesoft 의 ProxyVideoInfo 대체 (Phase 1 — width/height 만)
    struct ProxyVideoInfo
    {
        int width  = 0;
        int height = 0;
    };

    struct RtspSetting
    {
        std::string my_local_ip;
        /// 정적 init data 사용 여부 (true = init 시 등록된 카메라만, false = runtime 변경 허용)
        bool        is_rtsp_proxy_static_with_init = true;
        /// Phase 2: output RTSP server bind port (default 555, ServerSetting.RtspPort 로 override)
        std::string proxy_server_port = "555";
    };

    enum class RtspState { Create, Ready, Run, Stop };

    /**
     * @brief Phase 2 GStreamer 기반 RTSP handler.
     *
     * happytimesoft 외부 라이브러리 (Protocol/RTSP) 를 대체.
     *   - 입력 측: 카메라별 GstRtspClient (rtspsrc → avdec → appsink)
     *   - 출력 측: GstRtspProxyServer 단일 인스턴스 + per-cam mount point + ONVIF metadata payloader
     */
    class RtspHandler
    {
    public:
        RtspHandler( void ) = delete;
        explicit RtspHandler( const RtspSetting& setting ) noexcept;

        ~RtspHandler();

        RtspHandler( const RtspHandler& )            = delete;
        RtspHandler& operator=( const RtspHandler& ) = delete;
        RtspHandler( RtspHandler&& )                 = delete;
        RtspHandler& operator=( RtspHandler&& )      = delete;

        // 라이프사이클
        bool Initialize( void );                              // gst_init + proxy_server Start
        bool RunRTSP( void );                                 // state Ready -> Run
        bool StopRTSP( const int stop_timeout_seconds = 10 ); // 모든 client + proxy_server Stop
        bool RunProxiesAfterRuntimeSettingLoad( void );       // Phase 1: no-op

        // get
        bool IsProxySettingStaticUseInitData( void ) const;
        std::set<int> GetProxyIDs( void ) const;
        GstRtspClient* GetProxyPtr( const int cam_id ) const;
        std::optional<ProxyVideoInfo> GetProxyInfo( const int cam_id ) const;

        /**
         * @brief 외부 (RtspDetectorUnit) 가 NPU 감지 결과 XML 을 ONVIF metadata stream 으로 송출.
         * @return false = payloader 미생성 (클라이언트 미접속) 또는 push 실패. 호출자 무시 가능.
         */
        bool SendOnvifMetadata( const int cam_id, const std::string& xml_document );

        /**
         * @brief 입력 측 receiver 에서 받은 H.264 RTP packet 을 proxy server 의 video stream 으로 forward.
         *        GstRtspClient::SetRawPacketCallback 의 callback 에서 호출.
         *        Receiver pipeline: rtspsrc → depay → h264parse → tee → rtph264pay → raw_sink (RTP packet).
         * @return false = video_src 미생성 (클라이언트 미접속) 또는 push 실패. 호출자 무시 가능.
         */
        bool ForwardVideoRtp( const int cam_id, const uint8_t* data, std::size_t size ) noexcept;

        /**
         * @brief 입력 측 카메라 등록 + 출력 측 mount point 등록.
         *        on_sink callback 에서 GstAppSrc(video,meta) 핸들 보관 → 외부 push 경로 확보.
         */
        void RegisterClient( const int cam_id, std::unique_ptr<GstRtspClient> client ) noexcept;
        void UnregisterClient( const int cam_id ) noexcept;

    private:
        void SetRtspState( const RtspState state ) noexcept;

        /// proxy_server media-configure 시그널이 호출하는 콜백 — sink 보관 + payloader 생성.
        void OnProxySinkReady( const GstRtspProxyServer::CameraSink& sink );

    private:
        std::string my_local_ip_                    = "";
        bool        is_init_done_                   = false;
        bool        is_rtsp_proxy_static_with_init_ = true;
        std::string proxy_server_port_              = "555";
        RtspState   state_                          = RtspState::Create;

        mutable std::mutex                                clients_mtx_;
        std::map<int, std::unique_ptr<GstRtspClient>>     clients_;

        // Phase 2: 출력 측 RTSP 서버 + per-cam sink/payloader.
        std::unique_ptr<GstRtspProxyServer>               proxy_server_;

        /// per-cam: video appsrc (raw RTP forward 용) + onvif metadata payloader.
        struct CamSink
        {
            GstAppSrc*                              video_src = nullptr;  ///< weak — factory owns
            std::unique_ptr<OnvifMetadataPayloader> meta_payloader;       ///< wraps pay1 appsrc
        };
        mutable std::mutex                                cam_sinks_mtx_;
        std::map<int, CamSink>                            cam_sinks_;
    };

} // namespace MGEN
