#pragma once

#include "GstRtspClient.h"   // Protocol/RTSP_GST

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
    };

    enum class RtspState { Create, Ready, Run, Stop };

    /**
     * @brief Phase 1 GStreamer 기반 RTSP handler.
     *
     * happytimesoft 외부 라이브러리 (Protocol/RTSP) 를 대체. 카메라별
     * GstRtspClient 를 보유하고 라이프사이클을 관리. RtspDetectorUnit 이
     * 카메라 등록 시 RegisterClient 호출.
     *
     * Phase 3 에서 RTSP proxy server (분석 결과 출력) 가 추가 예정.
     */
    class RtspHandler
    {
    public:
        RtspHandler( void ) = delete;
        explicit RtspHandler( const RtspSetting setting ) noexcept;

        ~RtspHandler();

        /// 비복사/비이동 (shared_ptr 단일 소유)
        RtspHandler( const RtspHandler& )            = delete;
        RtspHandler& operator=( const RtspHandler& ) = delete;
        RtspHandler( RtspHandler&& )                 = delete;
        RtspHandler& operator=( RtspHandler&& )      = delete;

        // 라이프사이클
        bool Initialize( void );                              // gst_init (idempotent)
        bool RunRTSP( void );                                 // state Ready -> Run
        bool StopRTSP( const int stop_timeout_seconds = 10 ); // 모든 client Stop + clear
        bool RunProxiesAfterRuntimeSettingLoad( void );       // Phase 1: no-op

        // get
        bool IsProxySettingStaticUseInitData( void ) const;
        std::set<int> GetProxyIDs( void ) const;
        GstRtspClient* GetProxyPtr( const int cam_id ) const;
        std::optional<ProxyVideoInfo> GetProxyInfo( const int cam_id ) const;

        // 카메라 등록 (RtspDetectorUnit 이 자기 GstRtspClient 생성 후 register)
        void RegisterClient( const int cam_id, std::unique_ptr<GstRtspClient> client ) noexcept;
        void UnregisterClient( const int cam_id ) noexcept;

    private:
        void SetRtspState( const RtspState state ) noexcept;

    private:
        std::string my_local_ip_                    = "";
        bool        is_init_done_                   = false;
        bool        is_rtsp_proxy_static_with_init_ = true;
        RtspState   state_                          = RtspState::Create;

        mutable std::mutex                                clients_mtx_;
        std::map<int, std::unique_ptr<GstRtspClient>>     clients_;
    };

} // namespace MGEN
