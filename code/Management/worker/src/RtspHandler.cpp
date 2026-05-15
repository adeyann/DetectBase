#include "RtspHandler.h"

#include "MgenLogger.h"
#include "SettingManager.h"
#include "file_utils.h"

#include "json/json.hpp"
#include "xml/tiny_xml.h"

#include <libavformat/avformat.h>

#include <chrono>
#include <future>
#include <random>

extern RTSP_CLASS hrtsp;

namespace MGEN
{
    // ==================================================================================================================
    //   RtspHandler
    // ==================================================================================================================

    RtspHandler::RtspHandler( const RtspSetting setting ) noexcept
        : my_local_ip_                    ( setting.my_local_ip )
        , rtsp_xml_path_                  ( setting.rtsp_xml_path )
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
        if( this->is_init_done_ == true )
            return true;

		if( this->is_rtsp_proxy_static_with_init_ ) {
			if( IsValidFile( this->rtsp_xml_path_ ) == false ) {
				MLOG_ERROR("RTSP XML file ( %s ) is invalid file", this->rtsp_xml_path_.c_str() );
				return false;
			}
		}

        if( sys_buf_init( MAX_NUM_RUA * 2 ) == false ) {
            MLOG_ERROR( "%s() => sys_buf_init() Failed", __func__ );
            return false;
        }

		#if LIBAVFORMAT_VERSION_MAJOR < 58
		MLOG_DEBUG( "FFmpeg Version < 4.0 detected : Manually registering all formats and codecs" );
		av_register_all();
		avformat_network_init();
		#endif

        av_log_set_level( AV_LOG_QUIET );

    	srand( static_cast<uint32>( time( NULL ) ) );
        hrtsp.session_timeout = 60;

        this->SetRtspState( RtspState::Ready );

        is_init_done_ = true;
        return is_init_done_;
    }

	bool RtspHandler::IsProxySettingStaticUseInitData( void ) const
	{
		return this->is_rtsp_proxy_static_with_init_;
	}

    bool RtspHandler::RunRTSP( void )
    {
        if( this->state_ == RtspState::Run ) {
            if( this->StopRTSP() == false ) {
                MLOG_ERROR("Attempt to terminate an existing running or waiting RTSP failed");
                MLOG_ERROR("Canceling RunRTSP()");
                return false;
            }
        }

        if( IsValidFile( this->rtsp_xml_path_ ) == false ) {
			MLOG_ERROR("Try RunRTSP(), but xml file ( %s ) is invalid file", this->rtsp_xml_path_.c_str() );
            return false;
		}

        rtsp_read_config( this->rtsp_xml_path_.c_str() );

        memset( &hrtsp, 0, sizeof( hrtsp ) );

        if( g_rtsp_cfg.serverip_nums == 0 ) {
            g_rtsp_cfg.serverip_nums++;
            strcpy( g_rtsp_cfg.serverip[0], this->my_local_ip_.c_str() );
        }

        if( g_rtsp_cfg.serverport <= 0 || g_rtsp_cfg.serverport > 65535 )
		    g_rtsp_cfg.serverport = 556;

        sprintf( hrtsp.srv_ver, "MgenSolutions RTSP server %d.%d", 1, 0 );
        rua_proxy_init();

        hrtsp.msg_queue = hqCreate( MAX_NUM_RUA * 4, sizeof( RIMSG ), HQ_GET_WAIT );
        if( hrtsp.msg_queue == NULL ) {
            MLOG_ERROR( "%s() => create rtsp task queue failed", __func__ );
            return false;
        }

        hrtsp.ep_event_num = MAX_NUM_RUA + NET_IF_NUM + 8;
        hrtsp.ep_fd = epoll_create( hrtsp.ep_event_num );

        if( hrtsp.ep_fd < 0 ) {
            MLOG_ERROR( "%s() => epoll_create failed", __func__ );
            return false;
        }

        hrtsp.ep_events = ( struct epoll_event* )malloc( sizeof( struct epoll_event ) * hrtsp.ep_event_num );
        if( hrtsp.ep_events == NULL ) {
            MLOG_ERROR( "%s() => epoll malloc failed", __func__ );
            return FALSE;
        }

        for( int i = 0; i < g_rtsp_cfg.serverip_nums; i++ ) {
            int idx = hrtsp.local_ip_num;

            hrtsp.local_ip[idx] = get_address_by_name( g_rtsp_cfg.serverip[i] );
            strcpy( hrtsp.local_ipstr[idx], g_rtsp_cfg.serverip[i] );

            if( rtsp_net_listen_init( hrtsp.local_ip[idx], g_rtsp_cfg.serverport, idx ) < 0 ) {
                struct in_addr in {};
                in.s_addr = hrtsp.local_ip[idx];
                MLOG_ERROR( "%s() => rtsp_net_listen_init failed ( %s )", __func__, hrtsp.local_ip[idx] );
                MLOG_ERROR( "%s() => Bind %s:%d failed", __func__, inet_ntoa( in ), g_rtsp_cfg.serverport );
            }
            else { hrtsp.local_ip_num++; }
        }

        int bufs = MAX_NUM_RUA * 2;
        bufs += rtsp_get_proxy_nums() * 2;

        rtsp_parse_buf_init( bufs );
        rtsp_print_info();

		std::random_device rd;
		std::mt19937 gen(rd());
		std::uniform_int_distribution<> dis( 50, 400 );

		const bool is_start_conn_rtsp_with_init = this->IsProxySettingStaticUseInitData();
		auto       rtsp_start_delay_ms = std::chrono::milliseconds( 10 );

		std::unique_lock<std::mutex> lck { this->proxy_mtx_ };
        RTSP_PROXY* p_proxy = g_rtsp_cfg.proxy;
        while( p_proxy )
		{
            p_proxy->proxy = new CRtspProxy( &p_proxy->cfg );

			if( is_start_conn_rtsp_with_init ){
	            p_proxy->proxy->startConn( p_proxy->cfg.url, p_proxy->cfg.user, p_proxy->cfg.pass );

				int rand_ms = dis(gen);
				rtsp_start_delay_ms = std::chrono::milliseconds( rand_ms );
			}

			this_thread::sleep_for( rtsp_start_delay_ms );
            p_proxy = p_proxy->next;
        }
		lck.unlock();

        hrtsp.r_flag        = 1;
        hrtsp.tid_pkt_rx    = sys_os_create_thread( reinterpret_cast<void*>( rtsp_rx_thread ), NULL );
        hrtsp.tid_main      = sys_os_create_thread( reinterpret_cast<void*>( rtsp_task ), NULL );
        hrtsp.sys_init_flag = 1;

        this->SetRtspState( RtspState::Run );
        return true;
    }

    bool RtspHandler::RunProxiesAfterRuntimeSettingLoad( void )
	{
		if( this->IsProxySettingStaticUseInitData() == true )
			return false;

		std::random_device rd;
		std::mt19937 gen(rd());
		std::uniform_int_distribution<> dis( 50, 400 );

		std::unique_lock<std::mutex> lck { this->proxy_mtx_ };
        RTSP_PROXY* p_proxy = g_rtsp_cfg.proxy;
        while( p_proxy )
		{
			p_proxy->proxy->startConn( p_proxy->cfg.url, p_proxy->cfg.user, p_proxy->cfg.pass );

			int rand_ms = dis(gen);
            this_thread::sleep_for( chrono::milliseconds( rand_ms ) );

            p_proxy = p_proxy->next;
        }
		lck.unlock();

		return true;
	}

    bool RtspHandler::StopRTSP( const int stop_timeout_seconds )
    {
        if( this->state_ == RtspState::Run ){
            try {
                auto future = std::async( std::launch::async, []() {
                    return rtsp_stop();
                } );

                if( future.wait_for(std::chrono::seconds(stop_timeout_seconds)) == std::future_status::timeout ){
                    MLOG_WARN("   - Try Terminate All Camera Proxy, But Timeout: %d Seconds", stop_timeout_seconds);
                    this->SetRtspState( RtspState::Stop );
                    return false; // timeout 발생 시 실패로 간주
                }
                bool result = future.get(); // 결과값 안전하게 꺼냄
                if( result == true ) {
                    this->SetRtspState( RtspState::Stop );
                }
				MLOG_INFO("   - Terminate All Camera Proxy in RTSP");
                return true;
            }
            catch( const std::exception& e ){
                MLOG_ERROR("   - Exception during rtsp_stop(): %s", e.what());
                return false;
            }
        }
        return true; // 실행할 필요 없는 상태에서는 true 반환
    }

    bool RtspHandler::SetNewRtspConfigXml( const std::string& xml_path )
    {
        if( IsValidFile( xml_path ) == false )
            return false;

        this->rtsp_xml_path_ = xml_path;
        return true;
    }

    void RtspHandler::SetRtspState( const RtspState state ) noexcept
    {
        this->state_ = state;
    }

    std::set<int> RtspHandler::GetProxyIDs( void ) const
    {
        std::set<int> id_set;

		std::lock_guard<std::mutex> lck { this->proxy_mtx_ };

        RTSP_PROXY* p_proxy = g_rtsp_cfg.proxy;
        while( p_proxy ) {
            if( p_proxy->proxy ) {
                id_set.insert( p_proxy->proxy->getProxyID() );
            }
            p_proxy = p_proxy->next;
        }
        return id_set;
    }

    CRtspProxy* RtspHandler::GetProxyPtr( const int proxy_id ) const
    {
		std::lock_guard<std::mutex> lck { this->proxy_mtx_ };

        RTSP_PROXY* p_proxy = g_rtsp_cfg.proxy;
        while( p_proxy ) {
            CRtspProxy* ptr = p_proxy->proxy;
            if( ptr != nullptr && ptr->getProxyID() == proxy_id ) {
                return ptr;
            }
            p_proxy = p_proxy->next;
        }
        return nullptr;
    }

    std::optional<ProxyVideoInfo> RtspHandler::GetProxyInfo( const int proxy_id ) const
    {
        CRtspProxy* ptr = this->GetProxyPtr( proxy_id );
        if( ptr == nullptr )
            return std::nullopt;
        return ptr->getProxyVideoInfo();
    }

    // ==================================================================================================================
    //   RtspProxyConfigXmlMaker
    // ==================================================================================================================

    using nlohmann::json;

	static void InsertElement( TiXmlElement* insertTarget, const char* elemName, const char* elemValue )
	{
		// TiXmlElement 는 소멸자에 자체적으로 delete 가 있기 때문에 delete 하면 double free error
		auto* pElem = new TiXmlElement( elemName );

		if( elemValue != nullptr )
			pElem->LinkEndChild( new TiXmlText( elemValue ) );

		insertTarget->LinkEndChild( pElem );
	}

    static void InsertElement( TiXmlElement* insertTarget, const char* elemName, const int elemValue )
	{
		auto* pElem = new TiXmlElement( elemName );

		std::string strValue = std::to_string( elemValue );
		pElem->LinkEndChild( new TiXmlText( strValue.c_str() ) );

		insertTarget->LinkEndChild( pElem );
	}


    bool RtspProxyConfigXmlMaker::Make( const std::string& xml_result_save_path )
	{
        auto setting_manager = MGEN::GetSettingManager();

		// Get RTSP PORT from DETECTOR ServerSetting
		auto opt_nm_server_setting = setting_manager->GetServerSetting();

		int rtsp_port = DefineDefault::RTSP_PROXY_PORT;

		if( opt_nm_server_setting.has_value() == true ){
			if( rtsp_port != opt_nm_server_setting->rtsp_proxy_publish_port ) {
				MLOG_INFO("   - RTSP Proxy Port Changed From DB Setting. ( %d )",
					opt_nm_server_setting->rtsp_proxy_publish_port );
			}
			rtsp_port = opt_nm_server_setting->rtsp_proxy_publish_port;
		}

		auto sptrCamInfo = setting_manager->GetCameraSettingsManager();
		if( !sptrCamInfo ) {
			MLOG_ERROR( "%s() => CameraSettingManager not registered.", __func__ );
			return false;
		}

		// Xml Init
		TiXmlDocument doc;

		// Add Root node
		auto* pRoot = new TiXmlElement( "config" );
		doc.LinkEndChild( pRoot );

		// <config>
		InsertElement( pRoot, "serverip",       nullptr );
		InsertElement( pRoot, "serverip",       nullptr );
		InsertElement( pRoot, "serverport",     rtsp_port );
		InsertElement( pRoot, "loop_nums",      -1 );
		InsertElement( pRoot, "multicast",      0 );
		InsertElement( pRoot, "metadata",       1 );
		InsertElement( pRoot, "rtsp_over_http", 1 );
		InsertElement( pRoot, "http_port",      80 );
		InsertElement( pRoot, "need_auth",      0 );
		InsertElement( pRoot, "log_enable",     0 );
		InsertElement( pRoot, "log_level",      4 );

		// <config><user>
		auto* pAdmin = new TiXmlElement( "user" );
		InsertElement( pAdmin, "username", "admin" );
		InsertElement( pAdmin, "password", "admin" );
		pRoot->LinkEndChild( pAdmin );
		// <config></user>

		// <config><user>
		auto* pUser = new TiXmlElement( "user" );
		InsertElement( pUser, "username", "user" );
		InsertElement( pUser, "password", "123456" );
		pRoot->LinkEndChild( pUser );
		// <config></user>

		// <config><output>
		auto* output_H264 = new TiXmlElement( "output" );
		InsertElement( output_H264, "url", "screenlive" );

		// <config><output><video>
		auto* video_H264 = new TiXmlElement( "video" );
		InsertElement( video_H264, "codec", "H264" );
		InsertElement( video_H264, "width", nullptr );
		InsertElement( video_H264, "height", nullptr );
		InsertElement( video_H264, "framerate", 30 );
		InsertElement( video_H264, "bitrate", nullptr );
		output_H264->LinkEndChild( video_H264 );
		// <config><output></video>

		// <config><output><audio>
		auto* audio_H264 = new TiXmlElement( "audio" );
		InsertElement( audio_H264, "codec", "G711U" );
		InsertElement( audio_H264, "samplerate", 8000 );
		InsertElement( audio_H264, "channels", 1 );
		InsertElement( audio_H264, "bitrate", nullptr );
		output_H264->LinkEndChild( audio_H264 );
		// <config><output></audio>

		pRoot->LinkEndChild( output_H264 );
		// <config></output>

		// <config><output>
		auto* output_H265 = new TiXmlElement( "output" );
		InsertElement( output_H265, "url", nullptr );

		// <config><output><video>
		auto* video_H265 = new TiXmlElement( "video" );
		InsertElement( video_H265, "codec", "H265" );
		InsertElement( video_H265, "width", nullptr );
		InsertElement( video_H265, "height", nullptr );
		InsertElement( video_H265, "framerate", 30 );
		InsertElement( video_H265, "bitrate", nullptr );
		output_H265->LinkEndChild( video_H265 );
		// <config><output></video>

		// <config><output><audio>
		auto* audio_H265 = new TiXmlElement( "audio" );
		InsertElement( audio_H265, "codec", "G711U" );
		InsertElement( audio_H265, "samplerate", nullptr );
		InsertElement( audio_H265, "channels", nullptr );
		InsertElement( audio_H265, "bitrate", nullptr );
		output_H265->LinkEndChild( audio_H265 );
		// <config><output></video>

		pRoot->LinkEndChild( output_H265 );
		// <config></output>

		// GET StaticTotalManager
		const auto cam_set = setting_manager->GetCameraIDSet();

		for( const int cam_id : cam_set ) {

			// Get camera detail settings
			auto optCamSetting = sptrCamInfo->GetSetting( cam_id );
            if( optCamSetting.has_value() == false ) {
				MLOG_ERROR( "%s() => CAM[%d] exists camList, but has not detail settings...", __func__, cam_id );
                return false;
            }
            auto camSetting = *optCamSetting;

			if( camSetting.IsEmpty() ) {
				MLOG_ERROR( "%s() => CAM[%d] exists camList, but has not detail settings...", __func__, cam_id );
				return false;
			}
			if( camSetting.camera_id != cam_id ) {
				MLOG_ERROR( "%s() => CAM[%d] exists camList, but detail info cam_id unmatched ( %d != %d )", __func__, cam_id, cam_id, camSetting.camera_id );
				return false;
			}

			// <config><proxy>
			auto* proxy = new TiXmlElement( "proxy" );
			InsertElement( proxy, "suffix", camSetting.camera_id );
			InsertElement( proxy, "url",    camSetting.url.c_str() );
			InsertElement( proxy, "ip",     camSetting.access_ip.c_str() );
			InsertElement( proxy, "user",   camSetting.access_id.c_str() );
			InsertElement( proxy, "port",   nullptr ); // accessPort? 현재는 그런데 nullptr 값이었음
			InsertElement( proxy, "group",  nullptr );
			InsertElement( proxy, "vms",    nullptr );
			InsertElement( proxy, "pass",   camSetting.access_pw.c_str() );
			InsertElement( proxy, "delay",  20 );
			InsertElement( proxy, "guid",   nullptr );

			// <config><proxy><detect>
			auto* detect = new TiXmlElement( "detect" );
			InsertElement( detect, "codec",      "JPG" );
			InsertElement( detect, "samplerate", 999 );
			InsertElement( detect, "width",      640 );
			InsertElement( detect, "height",     640 );
			proxy->LinkEndChild( detect );
			// <config><proxy></detect>

			// <config><proxy><output>
			auto* proxy_output = new TiXmlElement( "output" );

			// <config><proxy><output><video>
			auto* proxy_video = new TiXmlElement( "video" );
			InsertElement( proxy_video, "codec",     nullptr );
			InsertElement( proxy_video, "width",     nullptr );
			InsertElement( proxy_video, "height",    nullptr );
			InsertElement( proxy_video, "framerate", nullptr );
			proxy_output->LinkEndChild( proxy_video );
			// <config><proxy><output></video>

			// <config><proxy><output><audio>
			auto* proxy_audio = new TiXmlElement( "audio" );
			InsertElement( proxy_audio, "codec",      nullptr );
			InsertElement( proxy_audio, "samplerate", nullptr );
			InsertElement( proxy_audio, "channels",   nullptr );
			InsertElement( proxy_audio, "bitrate",    nullptr );
			proxy_output->LinkEndChild( proxy_audio );
			// <config><proxy><output></audio>

			proxy->LinkEndChild( proxy_output );
			// <config><proxy></output>

			pRoot->LinkEndChild( proxy );
			// <config></proxy>
		}

		// <config><pusher>
		auto* pusher = new TiXmlElement( "pusher" );
		InsertElement( pusher, "suffix", "pusher" );

		// <config><pusher><video>
		auto* ps_video = new TiXmlElement( "video" );
		InsertElement( ps_video, "codec", "H264" );
		pusher->LinkEndChild( ps_video );
		// <config><pusher></video>

		// <config><pusher><audio>
		auto* ps_audio = new TiXmlElement( "audio" );
		InsertElement( ps_audio, "codec", "G711U" );
		InsertElement( ps_audio, "samplerate", 8000 );
		InsertElement( ps_audio, "channels",   1 );
		pusher->LinkEndChild( ps_audio );
		// <config><pusher></audio>

		// <config><pusher><transfer>
		auto* ps_trans = new TiXmlElement( "transfer" );
		InsertElement( ps_trans, "mode",  "UDP" );
		InsertElement( ps_trans, "ip",    nullptr );
		InsertElement( ps_trans, "vport", 50001 );
		InsertElement( ps_trans, "aport", 50002 );
		pusher->LinkEndChild( ps_trans );
		// <config><pusher></transfer>

		// <config><pusher><output>
		auto* ps_output = new TiXmlElement( "output" );

		// <config><pusher><output><video>
		auto* ps_out_video = new TiXmlElement( "video" );
		InsertElement( ps_out_video, "codec",     nullptr );
		InsertElement( ps_out_video, "width",     nullptr );
		InsertElement( ps_out_video, "height",    nullptr );
		InsertElement( ps_out_video, "framerate", nullptr );
		InsertElement( ps_out_video, "bitrate",   nullptr );
		ps_output->LinkEndChild( ps_out_video );
		// <config><pusher><output></video>

		// <config><pusher><output><audio>
		auto* ps_out_audio = new TiXmlElement( "audio" );
		InsertElement( ps_out_audio, "codec",      nullptr );
		InsertElement( ps_out_audio, "samplerate", nullptr );
		InsertElement( ps_out_audio, "channels",   nullptr );
		InsertElement( ps_out_audio, "bitrate",    nullptr );
		ps_output->LinkEndChild( ps_out_audio );
		// <config><pusher><output></audio>

		pusher->LinkEndChild( ps_output );
		// <config><pusher></output>

		pRoot->LinkEndChild( pusher );
		// <config></pusher>

		// <config><backchannel>
		auto* backchannel = new TiXmlElement( "backchannel" );
		InsertElement( backchannel, "codec",      "G711U" );
		InsertElement( backchannel, "samplerate", 8000 );
		InsertElement( backchannel, "channels",   1 );
		pRoot->LinkEndChild( backchannel );
		// <config></backchannel>

		doc.SaveFile( xml_result_save_path.c_str() );
		return true;
	}

} // namespace MGEN
