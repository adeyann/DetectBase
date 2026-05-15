#include "SioHandler.h"
#include "MgenLogger.h"
#include "CorrelationContext.h"
#include "MetricsRegistry.h"  // P54: socketio_reconnect_total counter
#include "json/json_impl.h"

#include <cstdlib>  // std::getenv
#include <ctime>    // localtime_r
#include <iomanip>  // std::put_time

namespace MGEN
{
    using namespace sio;
    using tm_t = struct tm;

    std::string GetEventNameStringFromEventNameEnumValue( const SocketIO::EventName::DETECTOR tag )
    {
        using namespace SocketIO::EventName;
        switch( tag )
        {
        case DETECTOR::EventNotifycation: return std::string { "EventNotifycation" };
        case DETECTOR::ExceptionUpdate:   return std::string { "ExceptionUpdate" };
        case DETECTOR::ScheduleUpdate:    return std::string { "ScheduleUpdate" };
        }
        return std::string { "" };
    }


    void SioSetting::SetDefaultConnectionQuery( void ) noexcept
    {
        using namespace MGEN::DefineDefault;

        this->sio_root_namespace    = DEFAULT_SOCKET_IO_DEFAULT_NSP;
        this->sio_conn_source       = DEFAULT_SOCKET_IO_CONN_SOURCE;
        this->sio_conn_user_type    = DEFAULT_SOCKET_IO_USER_TYPE;
        this->sio_conn_service_type = DEFAULT_SOCKET_IO_SERVICE_TYPE;

        // Auth token: 환경변수 우선, 없으면 default 사용 (활용성 우선 — 호스트/컨테이너/secret 모두 호환)
        if( const char* env_token = std::getenv( "MGEN_SIO_AUTH_TOKEN" ); env_token != nullptr ) {
            this->sio_conn_auth_token = env_token;
        } else {
            this->sio_conn_auth_token = DEFAULT_SOCKET_IO_AUTH_TOEKN;
        }
    }

    // SioHandler ============================================================================================
    SioHandler::SioHandler( const SioSetting& setting ) noexcept
        : setting( setting )
        , conn_listener( this )
    {
        // SocketIO disconnect 시 emit_queue 가 무한 증가하지 않도록 capacity 한계.
        // 한계 초과 시 oldest message drop (최신 이벤트 우선).
        this->emit_queue.SetMaxSize( 1000 );
    }

    SioHandler::~SioHandler()
    {
        this->TerminateSocketIO();
    }

    static void AppendHttpIfNotEixst( std::string& origin )
    {
        const std::string prefix = "http://";
        // 시작이 "http://"로 되어 있는지 확인
        if (origin.rfind(prefix, 0) != 0) {  // rfind(prefix, 0) == 0 이면 시작이 prefix임
            origin = prefix + origin;
        }
    }

    bool SioHandler::Initialize()
    {
        // Set Connection Listener
        this->client.set_open_listener ( std::bind( &SioHandler::connection_listener::OnConnect, &conn_listener ) );
        this->client.set_close_listener( std::bind( &SioHandler::connection_listener::OnClose,   &conn_listener, placeholders::_1 ) );
        this->client.set_fail_listener ( std::bind( &SioHandler::connection_listener::OnFail,    &conn_listener ) );

        // Set connection address format
        char c_url[128] = { '\0', };
        snprintf( c_url, 128, "%s:%d%s", this->setting.sio_service_ip.c_str(),
            this->setting.sio_service_port, this->setting.sio_root_namespace.c_str() );

        // Set sio conn address
        string s_url( c_url );
        AppendHttpIfNotEixst( s_url );

        MLOG_DEBUG(" Conn info = %s", s_url.c_str() );

        // Set connection query
        map<string, string> query;
        query["connectSource"] = this->setting.sio_conn_source;
        query["userType"]      = this->setting.sio_conn_user_type;
        query["serviceType"]   = this->setting.sio_conn_service_type;
        query["userId"]        = to_string( this->setting.my_server_id );

        // Set conenction auth
        auto auth = sio::object_message::create();
        auth->get_map()["token"] = string_message::create( this->setting.sio_conn_auth_token );

        auto map_to_json_func = [] ( const map<string, string>& m ) {
            nlohmann::json j;
            for( const auto& [ key, value ] : m ) {
                j[key] = value;
            }
            return j;
        };
        MLOG_INFO("   - SocketIO Connection Query : %s", map_to_json_func( query ).dump().c_str() );
        MLOG_INFO("   - SocketIO Connection Auth  : %s", ( auth ) ? "UseToken" : "NotAuth" );

        this->client.set_logs_quiet();

        // Reconnect 정책 명시: 지수 백오프, unlimited attempts, 최대 지연 30초
        this->client.set_reconnect_attempts( 0 );          // 0 = unlimited
        this->client.set_reconnect_delay   ( 1000 );       // 1초 시작
        this->client.set_reconnect_delay_max( 30000 );     // 최대 30초 (지수 백오프 상한)
        this->client.set_reconnect_listener(
            [this]( unsigned attempt, unsigned delay_ms ) {
                MLOG_INFO( "SocketIO reconnect attempt #%u (delay %u ms) -> %s:%d",
                    attempt, delay_ms,
                    this->setting.sio_service_ip.c_str(),
                    this->setting.sio_service_port );
                // P54: reconnect 누적 (Prometheus exporter).
                MGEN::MetricsRegistry::Instance().IncrementCounter(
                    "detectbase_socketio_reconnect_total", {} );
            }
        );

        // Connect
        this->client.connect( s_url, query, auth );

        // Wait Connection...
        std::unique_lock<std::mutex> lck { this->lock };
        if( !conn_finish ) {
            if( this->cond.wait_for( lck, std::chrono::seconds( 10 ), [&] { return conn_finish; } ) ) {
                MLOG_INFO( "   - SocketIO Connection Successful : %s", s_url.c_str() );
            }
            else {
                MLOG_ERROR( "  - SocketIO Connection Faild, Timeout(10s) : %s", s_url.c_str() );
                return false;
            }
        }
        lck.unlock();

        // Get Current Socket
        this->current_socket = this->client.socket( this->setting.sio_root_namespace );

        // Regist Emit Control Thread
        this->emit_control_thread.SetThreadFunctions(
            // runner
            std::bind( &SioHandler::EmitControlThreadRunner, this ),
            // closer
            std::bind( &SioHandler::EmitControlThreadCloser, this )
        );

        // Start Emit Thread
        this->emit_control_thread.Start();

        return true;
    }

    void SioHandler::TerminateSocketIO()
    {
        if( b_terminated_instance == true ) return;

        this->emit_control_thread.Stop();

        this->client.sync_close();
        this->client.clear_con_listeners();

        b_terminated_instance = true;
    }

    void SioHandler::RegistEvent( std::shared_ptr<SioEventBinder> event_binder )
    {
        // register event binder
        std::string event_name = event_binder->GetEventName();
        this->event_binders[event_name] = event_binder;

        this->current_socket->on( event_name,
            sio::socket::event_listener_aux( [&] ( string const& recv_event_name, message::ptr const& data, bool isAck, message::list& ack_resp )
            {
                // P53: 한 SocketIO 이벤트의 lifecycle 추적용 correlation_id 부여.
                // 이 scope 안에서 호출되는 모든 동기 chain 의 MLOG 가 동일 ID 출력.
                MGEN::CorrelationScope corr_scope { "evt" };

                MLOG_INFO( "Event (%s) update reqeust occred.", recv_event_name.c_str() );

                if( this->event_binders.find( recv_event_name ) == this->event_binders.end() ) {
                    MLOG_ERROR("event binder not exist" );
                    return;
                }

                const nlohmann::json sio_js = createJson( data );
                // MLOG_INFO("Event Json : %s", sio_js.dump(2).c_str());

                if( isAck == true )
                {
                    nlohmann::json ack_data   = nlohmann::json::object();
                    ack_data["receivedEvent"] = recv_event_name;
                    ack_data["status"]        = "success";
                    ack_resp.push(createObject(ack_data));

                    MLOG_INFO( " * Ack response sended for event : %s", recv_event_name.c_str() );
                }

                this->event_binders[recv_event_name]->Run( sio_js );
            }
        ) );
        MLOG_INFO( "   - Event bind successful ( %s )", event_name.c_str() );
    }

    void SioHandler::Emit( const std::string& event_name, const nlohmann::json& data )
    {
        SocketIO::EmitMessage message { event_name, data };
        // F-F4-07: copy 대신 move (큰 JSON payload 의 unnecessary copy 회피).
        // 운영 가시성: 큐 가득 시 drop oldest 직전 메트릭 (race window 있지만 통계 영향 미미).
        if( emit_queue.size() >= 1000 ) {
            MGEN::MetricsRegistry::Instance().IncrementCounter(
                "detectbase_errors_total", { { "type", "emit_drop" } } );
        }
        this->emit_queue.enqueue_move( std::move( message ) );
    }

    void SioHandler::Emit( const SocketIO::EmitMessage& message )
    {
        // const& 시그니처 보존 (외부 호출자 호환). 내부에서 copy 1회 발생 — 외부 호출자가 move 가능 시 위 overload 사용 권장.
        if( emit_queue.size() >= 1000 ) {
            MGEN::MetricsRegistry::Instance().IncrementCounter(
                "detectbase_errors_total", { { "type", "emit_drop" } } );
        }
        this->emit_queue.enqueue_copy( message );
    }

    SioHandler::connection_listener::connection_listener( SioHandler* h )
        : handler( h )
    {
        //
    };

    void SioHandler::connection_listener::OnConnect( void )
    {
        std::unique_lock<std::mutex> lck { handler->lock };
        handler->cond.notify_all();
        handler->conn_finish = true;
    }

    void SioHandler::connection_listener::OnClose( const sio::client::close_reason& reason )
    {
        std::string str_close_reason = "Undefined";
        switch( reason ) {
            case sio::client::close_reason::close_reason_drop:   str_close_reason = "close_reason_drop";   break;
            case sio::client::close_reason::close_reason_normal: str_close_reason = "close_reason_normal"; break;
            default: break;
        }
        // For Program Vaild Quit Process
        MLOG_INFO( "   - SocketIO Closed : %s:%d ( Close method : %s )",
            handler->setting.sio_service_ip.c_str(),
            handler->setting.sio_service_port, str_close_reason.c_str() );
    }

    void SioHandler::connection_listener::OnFail( void )
    {
        MLOG_ERROR( "SocketIO Failed : %s:%d",
            handler->setting.sio_service_ip.c_str(),
            handler->setting.sio_service_port );
        // process 강제 종료 제거: sioclient 자동 재연결에 맡김.
        //   - exit(0) 호출은 destructor 미실행/cleanup 누락/exit code 0 (정상 인식) 위험 있음.
        //   - 영구 실패 처리는 운영 정책 결정 후 g_terminate_flag 설정 등으로 별도 처리.
    }

    void SioHandler::EmitControlThreadRunner( void )
    {
        using namespace std::chrono_literals;

        auto&  running = this->emit_control_thread.GetRunningFlag();
        while( running.load() == true )
        {
            // dequeue_wait_for 는 종료/타임아웃 시 std::nullopt 반환 (throw 없음)
            auto opt_message = this->emit_queue.dequeue_wait_for( 1s );
            if( !opt_message.has_value() ){
                continue; // 종료 또는 타임아웃 → running 재검사
            }

            const auto& [ event_name, event_data ] = *opt_message;
            if( event_data.is_object() ){
                this->current_socket->emit( event_name, createObject( event_data ) );
            }
            else if( event_data.is_array() ){
                this->current_socket->emit( event_name, createArray( event_data ) );
            }
            else {
                MLOG_ERROR( "[EMIT] EventName : %s\n%s", event_name.c_str(), event_data.dump( 2 ).c_str() );
            }
        }
    }

    void SioHandler::EmitControlThreadCloser( void )
    {
        this->emit_queue.terminate();
        MLOG_INFO( "   - SocketIO Emit Msg Queue Flushed ( remains : %d )", static_cast<int>( this->emit_queue.size() ) );
    }

    // SioEventBinder ============================================================================================
    SioEventBinder::SioEventBinder( const SocketIO::EventNameTag event_name_tag, const SocketIO::EventType type )
        : event_name_tag( event_name_tag )
        , event_type    ( type )
    {
        this->str_event_name = GetEventNameStringFromEventNameEnumValue( event_name_tag );
        this->SetModeFlagUseEventType();
    }

    void SioEventBinder::SetModeFlagUseEventType( void ) noexcept
    {
        using SocketIO::EventType;
        switch( this->event_type )
        {
            case EventType::InterprotocolSettingUpdateTargetUnit:
            {
                this->need_interprotocol_mode         = true;
                this->need_setting_update_mode        = true;
                this->need_internal_enqueueing_mode   = false;
                this->need_target_unit_extractor_mode = true;
            }
            break;
        }
    }

    void SioEventBinder::SetInterProtocolFunc( InterProtocolFunc func )
    {
        this->inter_proc = func;
    }

    void SioEventBinder::SetTargetUnitsExtractor( TargetUnitsExtractor func )
    {
        this->update_units_extractor = func;
    }

    void SioEventBinder::SetInScopeDirectProcessor( InScopeDirectProcessor func )
    {
        this->in_scope_direct_processor = func;
    }

    void SioEventBinder::SetMultiUintsSettingManager( std::shared_ptr<ISettingManager> ptr )
    {
        this->multi_unit_setting_manager = ptr;
    }

    void SioEventBinder::SetInternalQueue( sptrSafeQueue<nlohmann::json> q )
    {
        this->internal_queue = q;
    }

    bool SioEventBinder::IsRunnable( const bool need_error_log )
    {
        bool success = true;
        // #01. Check need inter-protocol
        if( this->need_interprotocol_mode == true )
        {
            if( inter_proc == nullptr ) {
                if( need_error_log == true ) {
                    MLOG_ERROR("SioEventBinder( %s ), 'need_interprotocol_mode' == true, but 'inter_proc' == nullptr",
                        this->str_event_name.c_str() );
                }
                success = false;
            }
        }

        // #02-1. postprocess = setting manager update (target unit 단위)
        if( this->need_setting_update_mode == true )
        {
            if( this->update_units_extractor == nullptr ) {
                if( need_error_log == true ) {
                    MLOG_ERROR("SioEventBinder( %s ), 'need_setting_update_mode' == true, but 'update_units_extractor' == nullptr",
                        this->str_event_name.c_str() );
                }
                success = false;
            }
            if( this->multi_unit_setting_manager == nullptr ) {
                if( need_error_log == true ) {
                    MLOG_ERROR("SioEventBinder( %s ), 'need_setting_update_mode' == true, but 'multi_unit_setting_manager' == nullptr",
                        this->str_event_name.c_str() );
                }
                success = false;
            }
        }
        // #02-2. postprocess = internal queue enqueueing
        else if ( this->need_internal_enqueueing_mode == true )
        {
            if( this->internal_queue == nullptr ) {
                if( need_error_log == true ) {
                    MLOG_ERROR("SioEventBinder( %s ), 'need_internal_enqueueing_mode', but 'internal_queue' == nullptr",
                        this->str_event_name.c_str() );
                }
                success = false;
            }
        }
        // #02-3. postprocess = direct process in this Run() scope
        else
        {
            if( this->in_scope_direct_processor == nullptr ) {
                if( need_error_log == true ) {
                    MLOG_ERROR("SioEventBinder( %s ), direct process in this Run() scope, but 'in_scope_direct_processor' == nullptr",
                        this->str_event_name.c_str() );
                }
                success = false;
            }
        }
        return success;
    }

    bool SioEventBinder::Run( const nlohmann::json& js_sio )
    {
        nlohmann::json target_json {};

        // #01. Check need inter-protocol
        if( this->need_interprotocol_mode == true )
        {
            if( inter_proc == nullptr )
                return false;

            std::optional<nlohmann::json> resp = inter_proc( js_sio );
            if( resp.has_value() == false ) {
                MLOG_ERROR("Interproc Failed, event name = %s\n%s", str_event_name.c_str(), js_sio.dump(2).c_str() );
                return false;
            }
            // if need interprotocol preprocess, target_json is interprotocol(ex: REST API) return
            target_json = *resp;
        }
        else
        {
            // if NOT need interprotocol preprocess, target_json is socket.io json directly
            target_json = js_sio;
        }
        MLOG_DEBUG( "target_json :\n%s\n", target_json.dump( 2 ).c_str() );

        // #02-1. postprocess = setting manager update (target unit 단위)
        if( this->need_setting_update_mode == true )
        {
            if( this->update_units_extractor == nullptr || this->multi_unit_setting_manager == nullptr )
                return false;

            const auto update_target_units = this->update_units_extractor( js_sio );
            const auto api_rsp_filter_name = this->multi_unit_setting_manager->GetUnitKeyName();

            for( const auto& unit : update_target_units ) {
                const auto update_json = filterJsonArray( target_json, api_rsp_filter_name, unit );
                this->multi_unit_setting_manager->UpdateTargetUnit( unit, update_json );
            }
        }
        // #02-2. postprocess = internal queue enqueueing
        else if ( this->need_internal_enqueueing_mode == true )
        {
            if( this->internal_queue == nullptr )
                return false;
            this->internal_queue->enqueue_move( std::move( target_json ) );
        }
        // #02-3. postprocess = direct process in this Run() scope
        else
        {
            if( this->in_scope_direct_processor == nullptr )
                return false;
            this->in_scope_direct_processor( target_json );
        }
        return true;
    }

}
