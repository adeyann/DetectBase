#pragma once

#include "rtsp_srv.h"
#include "rtsp_cfg.h"
#include "rtsp_proxy.h"
#include "rtsp_timer.h"

#include <optional>
#include <set>
#include <mutex>
#include <string>

namespace MGEN
{
    struct RtspSetting
    {
        std::string my_local_ip;
        std::string rtsp_xml_path;
        bool        is_rtsp_proxy_static_with_init;
    };

    enum class RtspState { Create, Ready, Run, Stop, };

    class RtspProxyConfigXmlMaker
    {
    public:
        static bool Make( const std::string& xml_result_save_path );
    };

    class RtspHandler
    {
    public:
        // constructor
        RtspHandler( void ) = delete;
        explicit RtspHandler( const RtspSetting setting ) noexcept;

        // destructor
        ~RtspHandler();

        // initialize
        bool Initialize( void );

        // run
        bool RunRTSP( void );

        // stop
        bool StopRTSP( const int stop_timout_seconds = 10 );

        //
        bool RunProxiesAfterRuntimeSettingLoad( void );

        // set
        bool SetNewRtspConfigXml( const std::string& xml_path );

        // get
        bool IsProxySettingStaticUseInitData( void ) const;

        //
        std::set<int> GetProxyIDs( void ) const;

        CRtspProxy* GetProxyPtr( const int proxy_id ) const;

        std::optional<ProxyVideoInfo> GetProxyInfo( const int proxy_id ) const;

    private:
        void SetRtspState( const RtspState state ) noexcept;

    private:
        std::string my_local_ip_                    = "";
        std::string rtsp_xml_path_                  = "";
        bool        is_init_done_                   = false;
        bool        is_rtsp_proxy_static_with_init_ = true;
        RtspState   state_                          = RtspState::Create;

        // proxy mtx
        mutable std::mutex proxy_mtx_;
    };

} // namespace MGEN
