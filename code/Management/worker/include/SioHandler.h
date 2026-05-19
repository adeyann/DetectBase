#pragma once

// MGEN
#include "MgenLogger.h"
#include "SafeQueue.h"
#include "MgenTypes.h"
#include "InterProtocolTypes.h"
#include "SafeThread.h"
#include "ISettingManager.h"

// SocketIO
#include "sio_client.h"
#include "sio_parser.h"

// REST API Libs ( not yet )

// JSON
#include "json/json_fwd.hpp"

#include <mutex>
#include <thread>
#include <iostream>
#include <functional>
#include <condition_variable>
#include <string>
#include <ctime>
#include <iomanip>
#include <future>
#include <vector>
#include <atomic>

namespace MGEN
{
    namespace DefineDefault
    {
        DEFINE DEFAULT_SOCKET_IO_PORT         = 3335;
        DEFINE DEFAULT_REST_API_PORT          = 8005;
        DEFINE DEFAULT_SOCKET_IO_DEFAULT_NSP  = "/";
        DEFINE DEFAULT_SOCKET_IO_CONN_SOURCE  = "App";
        DEFINE DEFAULT_SOCKET_IO_USER_TYPE    = "Server";
        DEFINE DEFAULT_SOCKET_IO_SERVICE_TYPE = "DetectBase";
        DEFINE SERVER_IDENTIFIER_NOT_SET      = 0;
        DEFINE DEFAULT_TCP_SERVER_PORT        = 50000;
        DEFINE CAMERA_ID_UNIT_KEY             = "CameraId";
        DEFINE DEFAULT_SOCKET_IO_AUTH_TOEKN   = "detectbase-token";
    }

    namespace SocketIO::EventName
    {
        // DETECTOR 분기는 event name 자체는 "Message"로 통일하고,
        // 내부 JSON에서 MessageType으로 구분
        DEFINE DETECTOR_MESSAGE = "Message";

        enum class DETECTOR {
            EventNotifycation,
            ExceptionUpdate,
            ScheduleUpdate,
        };
    }

    namespace SocketIO
    {
        enum class TransmissionType : int
        {
            BroadCast = 1,
        };

        enum class EventType
        {
            // sio 수신 내용 기반 API 통신 후, 그 결과를 특정 unit 대상 setting update
            InterprotocolSettingUpdateTargetUnit,
        };

        using EventNameTag = SocketIO::EventName::DETECTOR;

        // eventName, eventData
        using EmitMessage = std::pair<std::string, nlohmann::json>;
    }

    std::string GetEventNameStringFromEventNameEnumValue( const SocketIO::EventName::DETECTOR tag );

    struct SioSetting
    {
        std::string sio_service_ip;        // target main server(mvas_broker) ip
        int         sio_service_port;      // target main server(mvas_broker) port
        int         my_server_id;          // current service identifier id
        std::string my_local_ip;           // current service local ip
        std::string sio_root_namespace;    // SocketIO connect root namespace ( default : / )
        std::string sio_conn_source;       // use for SocketIO connection query : connectSource
        std::string sio_conn_user_type;    // use for SocketIO connection query : userType
        std::string sio_conn_service_type; // use for SocketIO connection query : serviceType
        std::string sio_conn_auth_token;    // use for SocketIO connection auth : token

        void SetDefaultConnectionQuery( void ) noexcept;
    };

    class SioEventBinder;

    // TSan: callback (OnConnect/OnClose/OnFail) 가 SioHandler 소멸 후 호출되면 UAF.
    // sio::client 외부 lib 의 callback thread 가 sync_close() 이전에 진행 중일 수 있음.
    // enable_shared_from_this + weak_ptr capture 로 callback 안전화 (소멸 시 weak.lock() == nullptr → skip).
    class SioHandler : public std::enable_shared_from_this<SioHandler>
    {
    public:
        // Constructor : default constructor forbidden
        SioHandler() = delete;

        // Constructor only works that
        explicit SioHandler( const SioSetting& setting ) noexcept;

        // Destructor : must purpose sio client close
        ~SioHandler();

        // Initialize must be exec when program initialize
        bool Initialize();

        void RegistEvent( const std::shared_ptr<SioEventBinder>& event_binder );

        /** Function that is identified as an event and send
         * @param event_name : Bind Nmae about target event notify ( default : "Message" )
         * @param data       : Actual information about the event to be converted to sio message json data
         */
        void Emit( const std::string& event_name, const nlohmann::json& data );

        void Emit( const SocketIO::EmitMessage& message );

        // TerminateSocketIO = 영구 종료. emit_thread Stop + sio::client::sync_close +
        // listener clear. b_terminated_instance flag 로 idempotent 보장. destructor 에서 자동 호출.
        void TerminateSocketIO();

        int GetID( void ) { return this->setting.my_server_id; }

    private:
        // sio->Emit() control thread, for thread-safe socket communication
        void EmitControlThreadRunner( void );
        void EmitControlThreadCloser( void );

    private:
        // inner class in SioHandler
        class connection_listener {
        public:
            // constructor
            connection_listener( SioHandler* h );

            // bind handlers (registered via std::bind in SioHandler::Initialize)
            void OnConnect( void );
            void OnClose  ( const sio::client::close_reason& reason );
            void OnFail   ( void );

        private:
            SioHandler* handler;
        };

    private:
        // const init setting
        const SioSetting setting;

        // initialize connect succes check value
        bool conn_finish = false;

        // mutex values
        // NEW-8: condition_variable_any → condition_variable. 멤버 mutex 가 std::mutex 라 cv 충분.
        //        cv_any 는 generic mutex 지원 위해 자체 내부 mutex 가짐 → TSan lock-order-inversion +
        //        double lock false positive 18건 발생. cv 변경으로 해소 + 효율 ↑.
        std::condition_variable     cond;
        mutable std::mutex          lock;

        // conn
        sio::client client;
        sio::socket::ptr current_socket;
        SioHandler::connection_listener conn_listener;

        // Event Bind Map : for setting
        std::unordered_map<std::string, std::shared_ptr<SioEventBinder>> event_binders;

        // for emit
        SafeQueue<SocketIO::EmitMessage> emit_queue;

        // emit thread
        MGEN::SafeThread emit_control_thread;

        // for exit
        bool b_terminated_instance = false;
    };

    class SioEventBinder
    {
    public:
        using TargetUnitsExtractor   = std::function<std::vector<MGEN::Type::UnitID>(const nlohmann::json&)>;
        using InScopeDirectProcessor = std::function<void(const nlohmann::json&)>;

        SioEventBinder() = delete;

        SioEventBinder( const SocketIO::EventNameTag event_name_tag, const SocketIO::EventType type );

        void SetInScopeDirectProcessor( InScopeDirectProcessor func );
        void SetTargetUnitsExtractor( TargetUnitsExtractor func );
        void SetInterProtocolFunc( InterProtocolFunc func );

        void SetMultiUintsSettingManager( std::shared_ptr<ISettingManager> ptr );
        void SetInternalQueue( sptrSafeQueue<nlohmann::json> q );

        std::string GetEventName( void ) const noexcept { return this->str_event_name; }

        bool IsRunnable( const bool need_error_log = false );
        bool Run( const nlohmann::json& js );

    private:
        void SetModeFlagUseEventType( void ) noexcept;

    private:
        // core
        SocketIO::EventNameTag     event_name_tag;
        SocketIO::EventType        event_type;
        std::string                str_event_name = "";

        // func
        InterProtocolFunc      inter_proc                = nullptr;
        TargetUnitsExtractor   update_units_extractor    = nullptr;
        InScopeDirectProcessor in_scope_direct_processor = nullptr;

        // dest
        std::shared_ptr<ISettingManager> multi_unit_setting_manager = nullptr;
        sptrSafeQueue<nlohmann::json>    internal_queue             = nullptr;

        bool need_interprotocol_mode         = false;
        bool need_setting_update_mode        = false;
        bool need_internal_enqueueing_mode   = false;
        bool need_target_unit_extractor_mode = false;
    };

}

