#include "DETECTOR.h"

#include "MgenLogger.h"
#include "MgenLoggerMacro.h"
#include "MetricsRegistry.h"   // P54: Prometheus exporter
#include "GrpcEventServerBase.h"  // Phase 3: GRPC server (조건부)
#include "file_utils.h"
#include "ProgramConfig.h"
#include "MgenSchedule.h"

#include "EngineHandlerBuilder_NPU.h"

namespace MGEN
{
    static void ShowSettingManagersTotalInfo( void )
    {
        auto setting_manager = GetSettingManager();
        auto camera_list     = setting_manager->GetCameraIDSet();

        MLOG_INFO("-------------------------------------------------------------------------------------------");
        MLOG_INFO("###. Server Configuration");

        const auto server_setting = setting_manager->GetServerSetting();
        if( server_setting.has_value() ) {
            MLOG_INFO("   - RTSP Proxy Sync Delay        : %d ms", server_setting->rtsp_proxies_sync_delay_ms );
            MLOG_INFO("   - RTSP Proxy Publish Port      : %d",    server_setting->rtsp_proxy_publish_port );
            MLOG_INFO("   - Inference Per Cam FPS Limit  : %d",    server_setting->inference_per_cams_fps_limit );
            MLOG_INFO("   - Tracking Object Memory Limit : %d",    server_setting->tracking_memory_object_limit );
            MLOG_INFO("   - Full Image Save Path         : %s",    server_setting->full_image_save_root_path.c_str() );
            MLOG_INFO("   - Crop Image Save Path         : %s",    server_setting->crop_image_save_root_path.c_str() );
        } else {
            MLOG_INFO("  [!] Server Setting NOT EXIST!");
        }
        MLOG_INFO("-------------------------------------------------------------------------------------------");
        MLOG_INFO("###. Camera Configuration ( Total Count : %zu )", camera_list.size());

        for( const auto& cam_id : camera_list ) {

            bool is_exclude = false;
            const auto exclude_setting = setting_manager->GetExcludeCamSetting( cam_id );
            if( exclude_setting.has_value() && exclude_setting->is_exclude ) {
                is_exclude = true;
            }

            std::string schedule_str = "";
            const auto sch_setting = setting_manager->GetScheduleSetting( cam_id );

            if( sch_setting.has_value() && !sch_setting->schedules.empty() ) {
                for( size_t i = 0; i < sch_setting->schedules.size(); ++i ) {
                    const auto& sch = sch_setting->schedules[i];
                    // 이벤트 코드를 문자열로 변환
                    schedule_str += Abnormal::Schedule::GetEventName( sch.event_code );
                    // 마지막 요소가 아니면 콤마 추가
                    if( i < sch_setting->schedules.size() - 1 ) {
                        schedule_str += ", ";
                    }
                }
            } else {
                schedule_str = "None";
            }

            if( is_exclude ) {
                MLOG_INFO("   - [%04d] EXCLUDED CAMERA", cam_id );
            }
            else {
                MLOG_INFO("   - [%04d] EVENT [ %s ]",
                    cam_id, schedule_str.c_str() );
            }
        }
    }

    Service_DETECTOR::Service_DETECTOR( void )
        : service_profile_( ServiceProfileBuilder::Build() )
        , service_version_( GetApplicationVersion() )
    {

    }

    Service_DETECTOR::~Service_DETECTOR()
    {
        this->Quit();
    }

    bool Service_DETECTOR::Initialize( void )
    {
        // 로그 스텝 초기화 (MgenLoggerMacro.h의 매크로 활용)
        STEP_RESET();

        // 서비스 시작 배너 (DETECTOR 스타일)
        MLOG_INFO( "===========================================================================================" );
        MLOG_INFO( "###. DetectBase - %s [ %s ] [ %s | NPU | %s ]",
            PROGRAM_DESCRIPTION, PROGRAM_SERVICE_TYPE,
            service_version_.c_str(), PROGRAM_BUILD_TYPE );

        // P54: Prometheus exporter 시작 (port 9090) + 메트릭 등록.
        // 운영 모니터링용 /metrics endpoint. 각 측정점에서 직접 update.
        {
            auto& m = MGEN::MetricsRegistry::Instance();
            m.Initialize( 9090 );

            // Detection FPS 합계 (모든 카메라 합산).
            m.RegisterGauge( "detectbase_dfps_total",
                "Total detection FPS across all cameras", {} );

            // 등록 / 활성 카메라 수. label=state (registered|active).
            m.RegisterGauge( "detectbase_camera_count",
                "Camera count by state (registered/active)", { "state" } );

            // 이벤트 감지 누적. label=type (LineIntrusion|VehicleIntrusion|AreaIntrusion|VehicleParking), cam.
            m.RegisterCounter( "detectbase_events_total",
                "Total intrusion events detected", { "type", "cam" } );

            // 에러 누적. label=type
            //   imwrite_fail / emit_fail / npu_fail / preprocess_fail / sio_disconnect
            //   engine_input_q_drop / emit_drop / io_work_drop / logger_fail (drop 메트릭, 운영 가시성)
            m.RegisterCounter( "detectbase_errors_total",
                "Total errors by category", { "type" } );

            // setting RenewAfterReset 부분 실패 (graceful degradation 메트릭).
            // unit_key = CameraId 등, unit_id = 실패한 unit 의 ID.
            m.RegisterCounter( "detectbase_setting_partial_failure_total",
                "Setting reset partial failures (per unit_id, graceful degradation)",
                { "unit_key", "unit_id" } );

            // SocketIO reconnect 누적.
            m.RegisterCounter( "detectbase_socketio_reconnect_total",
                "SocketIO reconnect attempts", {} );

            // P54 Layer 3: /frame 디스크 방어 정책 메트릭.
            m.RegisterGauge( "detectbase_frame_disk_used_bytes",
                "Bytes used on /frame mount", {} );
            m.RegisterGauge( "detectbase_frame_disk_capacity_bytes",
                "Total capacity of /frame mount", {} );
            m.RegisterGauge( "detectbase_frame_disk_used_pct",
                "Used percent of /frame mount", {} );
            m.RegisterCounter( "detectbase_imwrite_skipped_total",
                "imwrite skipped (preemptive disk-full guard)", { "reason" } );
            m.RegisterCounter( "detectbase_frame_cleanup_deleted_total",
                "Day folders deleted by regular (7-day retention) cleanup", {} );
            m.RegisterCounter( "detectbase_frame_emergency_cleanup_total",
                "Items deleted by emergency cleanup (disk >= 80%). type=day_dir or half_files", { "type" } );

            // GRPC client (Phase 1).
            m.RegisterGauge( "detectbase_grpc_client_enabled",
                "GRPC client enabled (1) or disabled (0)", {} );
            m.RegisterGauge( "detectbase_grpc_client_peer_count",
                "Number of active GRPC peers", {} );
            m.RegisterCounter( "detectbase_grpc_send_total",
                "GRPC RPC send attempts (fire-and-forget aggregate)", { "rpc" } );

            // GRPC server (Phase 3).
            m.RegisterGauge( "detectbase_grpc_server_enabled",
                "GRPC server enabled (1) or disabled (0)", {} );
            m.RegisterCounter( "detectbase_grpc_recv_total",
                "GRPC RPC receive count by type", { "rpc" } );

            // GRPC client send 결과 분리 (Phase 6).
            m.RegisterCounter( "detectbase_grpc_send_success_total",
                "GRPC RPC send completions with OK status", { "rpc" } );
            m.RegisterCounter( "detectbase_grpc_send_failed_total",
                "GRPC RPC send completions with FAIL status", { "rpc", "code" } );
        }

        // -------------------------------------------------------------------------
        // #01. Load Profiles & Settings
        // -------------------------------------------------------------------------
        STEP_START( "Load Engine Profiles & Settings..." );
        // Network Profile
        const std::string ns_path = GetNetworkProfileJsonPath();
        if( auto err_msg = NetworkProfileParser::CheckParsable( ns_path ); err_msg.has_value() ) {
            MLOG_ERROR("%s", err_msg.value().c_str() );
            return false;
        }
        if( auto opt = NetworkProfileParser::Parse( ns_path ); opt.has_value() ) {
            network_profile_ = std::move( *opt );
        }
        else {
            MLOG_ERROR( "NetworkProfileParser::Parse failed: %s", ns_path.c_str() );
            return false;
        }
        MLOG_INFO("   - Network Profile Loaded");

        // Engine Profile
        const std::string es_path = GetEngineProfilesJsonPath();
        if( auto err_msg = EngineProfileParser::CheckParsable( es_path ); err_msg.has_value() ) {
            MLOG_ERROR("%s", err_msg.value().c_str() );
            return false;
        }
        if( auto opt = EngineProfileParser::Parse( es_path ); opt.has_value() ) {
            engine_profiles_ = std::move( *opt );
        }
        else {
            MLOG_ERROR( "EngineProfileParser::Parse failed: %s", es_path.c_str() );
            return false;
        }

        // [DETECTOR Style] 엔진 리스트 출력 (EngineProfile::ShowProfileInfo 수정본이 호출됨)
        MLOG_INFO("   - Loaded Engine Profiles : %zu", engine_profiles_.size());
        ShowEngineProfiles( engine_profiles_ );
        STEP_DONE();

        // -------------------------------------------------------------------------
        // #02. Create Managers & Load Balancer
        // -------------------------------------------------------------------------
        STEP_START( "Initialize Core Managers..." );
        network_manager_ = std::make_shared<NetworkManager>( network_profile_ );
        STEP_CHECK( network_manager_ != nullptr );

        io_stream_manager_ = std::make_shared<IOStreamManager>();
        STEP_CHECK( io_stream_manager_ != nullptr );

        load_balancer_ = std::make_shared<EngineLoadBalancer>( engine_profiles_ );
        STEP_CHECK( load_balancer_ != nullptr );

        MLOG_INFO("   - NetworkManager, IOStreamManager, LoadBalancer Created");
        STEP_DONE();

        // -------------------------------------------------------------------------
        // #03. Load Inference Engines (GPU/NPU)
        // -------------------------------------------------------------------------
        STEP_START( "Load Inference Engines..." );
        EngineHandlerBuilder_NPU engine_builder;
        MLOG_INFO("   - Builder Type : NPU (RKNN)");

        engines_ = engine_builder.BuildHandlers( engine_profiles_, load_balancer_->GetEngineLinker() );

        if( engines_.empty() ){
            MLOG_ERROR("EngineHandlerBuilder.BuildHandlers() => empty.");
            return false;
        }
        MLOG_INFO("   - Total Active Engine Instance : %zu", engines_.size());
        STEP_DONE();

        // -------------------------------------------------------------------------
        // #04. Connect Network & RTSP
        // -------------------------------------------------------------------------
        STEP_START( "Connect Network Service..." );
        // MVAS connection
        if( network_manager_->ConnMVAS( service_profile_.GetServiceName() ) == false ) {
            MLOG_ERROR("NetworkManager::ConnMVAS FAILED");
            return false;
        }

        // RTSP Camera Setting Load
        if( network_manager_->InitializeRTSPWithStaticCameraList() == false ) {
            MLOG_ERROR("NetworkManager::InitializeRTSPWithStaticCameraList FAILED");
            return false;
        }
        MLOG_INFO("   - MVAS Connected & RTSP List Loaded");

        // GRPC client 활성/peer 수 메트릭 1회 update (이후 정적 — peer 변경 시점 없음).
        {
            auto& m = MGEN::MetricsRegistry::Instance();
            const bool enabled = network_manager_->IsGrpcClientEnabled();
            m.SetGauge( "detectbase_grpc_client_enabled",   {}, enabled ? 1.0 : 0.0 );
            m.SetGauge( "detectbase_grpc_client_peer_count", {},
                static_cast<double>( network_manager_->GetGrpcPeerCount() ) );
        }
        STEP_DONE();

        // -------------------------------------------------------------------------
        // #__. Show Setting Managers Total Info
        // -------------------------------------------------------------------------
        ShowSettingManagersTotalInfo();

        // -------------------------------------------------------------------------
        // #05. Initialize Service Blocks
        // -------------------------------------------------------------------------
        STEP_START( "Build Service Blocks..." );
        // IO Stream Ready
        if( io_stream_manager_->Ready( service_profile_, network_manager_ ) == false ) {
            MLOG_ERROR("IOStreamManager::Ready FAILED");
            return false;
        }

        // LoadBalancer Counter Start
        load_balancer_->StartInferenceCounter();

        // Engine Activation
        for( auto& engine : engines_ ) {
            if( engine->ActivateEngine() == false ) {
                MLOG_ERROR("engine(%d)->ActivateEngine() failed", engine->GetDeviceID() );
                return false;
            }
        }
        MLOG_INFO("   - All Engines Activated");

        // SocketIO Event Bind
        if( SocketIOEventBind() == false ){
            MLOG_ERROR("SocketIOEventBind() Failed");
            return false;
        }

        // Build Detector Block
        auto opt_rtsp_service_block_profile = service_profile_.GetBlockProfile( ServiceBlockModuleType::DETECTOR_RTSP );
        if( !opt_rtsp_service_block_profile.has_value() ) return false;

        detector_block_ = std::make_unique<RtspDetectorBlock>(
            *opt_rtsp_service_block_profile, network_manager_, io_stream_manager_, load_balancer_ );
        STEP_CHECK( detector_block_ != nullptr );

        if( detector_block_->BuildServiceUnit( 10ms ) == false ) return false;
        if( detector_block_->Init( 30ms ) == false ) return false;

        MLOG_INFO("   - Detector Block Built & Initialized");
        STEP_DONE();

        // Phase 3: GRPC server 활성 시 시작.
        // 분석 6 fix (Phase 2) 가 들어간 GrpcEventServerBase 사용. detached thread UAF 차단.
        if( network_profile_.grpc_server_enabled ) {
            STEP_START( "Start GRPC Server..." );
            // W-13: make_unique → make_shared. enable_shared_from_this 사용을 위해 shared_ptr 필수.
            grpc_server_ = std::make_shared<GrpcEventServerBase>(
                network_profile_.grpc_server_bind_address,
                static_cast<int>( network_profile_.grpc_server_port ) );

            // SendEventOnlyJson 수신 → 로그 출력 + 메트릭 ↑ (샘플 동작; 실제 프로덕트는 큐로 forward).
            grpc_server_->SetSendEventOnlyJsonPostProcesser(
                []( const EventDataOnlyJson& req, Empty& /*rsp*/ ){
#ifdef DEBUG_MODE
                    MLOG_INFO( "[GRPC RECV] SendEventOnlyJson uuid=%s json_len=%zu",
                        req.uuid().c_str(), req.json_data().size() );
#else
                    (void) req;
#endif
                    MGEN::MetricsRegistry::Instance().IncrementCounter(
                        "detectbase_grpc_recv_total", { { "rpc", "SendEventOnlyJson" } } );
                });

            grpc_server_->SetSendEventWithImagesPostProcesser(
                []( const EventDataWithRawImages& req, Empty& /*rsp*/ ){
#ifdef DEBUG_MODE
                    MLOG_INFO( "[GRPC RECV] SendEventWithImages uuid=%s images=%d",
                        req.uuid().c_str(), req.images_size() );
#else
                    (void) req;
#endif
                    MGEN::MetricsRegistry::Instance().IncrementCounter(
                        "detectbase_grpc_recv_total", { { "rpc", "SendEventWithImages" } } );
                });

            grpc_server_->Run();

            // 메트릭 enabled = 1.
            MGEN::MetricsRegistry::Instance().SetGauge(
                "detectbase_grpc_server_enabled", {}, 1.0 );
            MLOG_INFO("   - GRPC Server listening on %s:%u",
                network_profile_.grpc_server_bind_address.c_str(),
                network_profile_.grpc_server_port );
            STEP_DONE();
        } else {
            MGEN::MetricsRegistry::Instance().SetGauge(
                "detectbase_grpc_server_enabled", {}, 0.0 );
        }

        STEP_LINE;
        MLOG_INFO( "###. SERVICE START SUCCESS" );
        MLOG_INFO( "===========================================================================================" );

        return true;
    }

    bool Service_DETECTOR::Run( void )
    {
        auto rtsp_handler = network_manager_->GetRtspHandler();
        if( !rtsp_handler ){
            MLOG_ERROR("!RtspHandler");
            return false;
        }
        if( rtsp_handler->RunRTSP() == false ) {
            MLOG_ERROR("RtspHandler->RunRTSP() failed");
            return false;
        }

        if( detector_block_->Start( 30ms ) == false ) {
            MLOG_ERROR("RtspDetectorBlock Start failed");
            return false;
        }

        return true;
    }

    bool Service_DETECTOR::Quit( void )
    {
        if( is_quit_.exchange( true ) == false )
        {
            cond_.notify_all();

            // [DETECTOR Style] 종료 로그 통일
            MLOG_INFO( "===========================================================================================" );
            MLOG_INFO( "###. PROGRAM QUIT START" );

            STEP_RESET(); // 종료 시에도 Step 1부터 시작하도록 리셋

            // ============================================================================
            // !!! DO NOT REORDER !!! 검증된 종료 순서 — 변경 시 UAF 위험.
            // ============================================================================
            // 종료 순서: 원래 순서 유지 (Stage 15 ClearAllSubscriptions 적용 후 새 순서 재시도 예정).
            //            새 순서로 변경 시 SocketIO close 도중 setting callback 진행 중인 상태에서
            //            RtspDetectorUnit 이 그 후에 destroy 되어 UAF 발생.
            // 자세한 내용: logs/REVIEW2/INTER_FLOW.md §3, §7.
            // ============================================================================

            // #00. Stop GRPC Server (외부 client 의 새 요청 수신 차단).
            //      Phase 2 fix 덕분에 detached thread 가 처리 중이어도 안전 (shared_from_this).
            if( grpc_server_ ) {
                STEP_START( "Stop GRPC Server..." );
                grpc_server_->Stop();
                grpc_server_.reset();
                MLOG_INFO("   - GRPC Server Stopped");
                MGEN::MetricsRegistry::Instance().SetGauge(
                    "detectbase_grpc_server_enabled", {}, 0.0 );
                STEP_DONE();
            }

            // #01. Terminate Engines
            STEP_START( "Terminate Engines..." );
            for( auto& e : engines_ ) { e->TerminateEngine(); }
            MLOG_INFO("   - All Engine Handlers Terminated");
            STEP_DONE();

            // #02. Terminate Load Balancer
            STEP_START( "Terminate Load Balancer..." );
            if( load_balancer_ ) load_balancer_->Terminate();
            MLOG_INFO("   - Load Balancer Stopped");
            STEP_DONE();

            // #03. Stop Service Implements
            STEP_START( "Stop Service Implements..." );
            if( detector_block_ ) detector_block_->Stop();
            MLOG_INFO("   - Detector Block Stopped");
            STEP_DONE();

            // #04. Stop Network Flow
            STEP_START( "Stop Network Flow..." );
            if( network_manager_ ) network_manager_->CloseNetworkAll();
            MLOG_INFO("   - Network Connections Closed");
            STEP_DONE();

            // #05. Stop IO Stream Manager
            STEP_START( "Stop IO Stream Manager..." );
            if( io_stream_manager_ ) io_stream_manager_->ClearAll();
            MLOG_INFO("   - IO Manager Cleared");
            STEP_DONE();

            // P54: Prometheus exporter 종료. 측정 자체는 종료되지만 endpoint 만 닫힘.
            MGEN::MetricsRegistry::Instance().Shutdown();

            MLOG_INFO( "-------------------------------------------------------------------------------------------" );
            MLOG_INFO( "###. PROGRAM QUIT SUCCESS" );
            MLOG_INFO( "===========================================================================================" );
        }
        return true;
    }

    bool Service_DETECTOR::WaitUntilQuitSignal( std::atomic<bool>& signal_quit_flag )
    {
        using namespace std::chrono_literals;
        while( signal_quit_flag.load() == false ) {
            std::unique_lock<std::mutex> lck { this->mtx_ };
            cond_.wait_for( lck, 100ms, [&] { return is_quit_.load() == true; } );
            if( is_quit_.load() || signal_quit_flag.load() )
                break;
        }

        this->Quit();
        return true;
    }

    static bool CheckInterProcJsonValid( const nlohmann::json& json, const char* argument_key_name )
    {
        const bool has_table_name    = json.contains( "TableName" );
        const bool has_argument_name = json.contains( "Argument" );

        if( has_table_name == false || has_argument_name == false )
            return false;

        if( !json["Argument"].is_array() )
            return false;

        const auto argument = json.value("Argument", nlohmann::json::array() );
        if( argument.empty() )
            return false;

        for( const auto& object : argument ){
            if( !object.is_object() )
                continue;
            if( object.contains( argument_key_name ) == true ) {
                // 값이 array of (string|number) 인지 검증 (URL 인자 안전성)
                const auto& v = object[ argument_key_name ];
                if( !v.is_array() )
                    continue;
                bool all_scalar = true;
                // raw loop 가독성 우선.
                // cppcheck-suppress useStlAlgorithm
                for( const auto& e : v ) {
                    if( !e.is_string() && !e.is_number() ) { all_scalar = false; break; }
                }
                if( all_scalar )
                    return true;
            }
        }
        return false;
    }

    static std::string GetAPICommandFromJson( const nlohmann::json& json, const std::string& target_url = std::string { "" } )
    {
        /* [INPUT] ----------------------------------------------------
        * {
        *   "Type" : "GET"
        *   "CameraId" : [1, 2],
        *   "TableName" : "Exception",
        *   "Argument" : [
        *       { "CameraId": [1, 2] }, { "UserCode": [4, 6] }
        *   ]
        * }
        * [OUTPUT] ----------------------------------------------------
        * http://192.168.1.102:8000/Exception?CameraId=1,2&UserCode=4,6
        * ---------------------------------------------------------- */

        std::string fullAPI = "/";
        if( target_url.empty() ){
            fullAPI += json.value( "TableName", "" );
        } else {
            fullAPI += target_url;
        }

        if( json.contains( "Argument" ) == false ) // "Argument"
            return fullAPI;

        nlohmann::json jsArray = json.value( "Argument", nlohmann::json::array() );
        bool isFirst = true;
        for( const auto& elem : jsArray ) {
            for( json::const_iterator it = elem.begin(); it != elem.end(); ++it ) {
                fullAPI += ( isFirst ) ? "?" : "&";
                fullAPI += it.key() + "=";

                size_t arg_idx = 0;
                for( const auto& value : it.value() ) {
                    if( arg_idx++ != 0 ) fullAPI += ",";

                    if( value.is_string() ) {
                        const std::string s = value.get<std::string>();
                        try {
                            fullAPI += std::to_string( std::stoi( s ) );
                        } catch( ... ) {
                            MLOG_ERROR( "GetAPICommandFromJson: non-numeric string for key '%s': %s",
                                        it.key().c_str(), s.c_str() );
                            return std::string{}; // 빈 URL → ApiHandler 가 GET skip
                        }
                    }
                    else if( value.is_number() ) {
                        fullAPI += std::to_string( value.get<int>() );
                    }
                    else {
                        // 예상 외 타입 (array/object/bool/null) — broken request 차단
                        MLOG_ERROR( "GetAPICommandFromJson: unsupported value type for key '%s': %s",
                                    it.key().c_str(), value.dump().c_str() );
                        return std::string{};
                    }
                }
                isFirst = false;
            }
        }
        return fullAPI;
    }

    static std::vector<MGEN::Type::UnitID> ExtractUnitFromJson( const nlohmann::json& json, const char* argument_key_name )
    {
        const bool has_table_name    = json.contains( "TableName" );
        const bool has_argument_name = json.contains( "Argument" );

        if( has_table_name == false || has_argument_name == false )
            return std::vector<MGEN::Type::UnitID> {};

        const auto argument = json.value("Argument", nlohmann::json::array() );
        if( argument.empty() )
            return std::vector<MGEN::Type::UnitID> {};

        std::vector<MGEN::Type::UnitID> res {};
        for( const auto& object : argument ){
            if( !object.is_object() ) continue;
            if( object.contains( argument_key_name ) ) {
                const auto& units = object[argument_key_name];
                if( !units.is_array() ) continue;
                for( const auto& unit : units )
                {
                    if( unit.is_string() ) {
                        const std::string s = unit.get<std::string>();
                        try {
                            res.push_back( std::stoi( s ) );
                        } catch( const std::exception& e ) {
                            // 비숫자 string → 이 unit 만 skip (process crash 차단)
                            MLOG_ERROR( "ExtractUnitFromJson: non-numeric string for key '%s': %s",
                                        argument_key_name, s.c_str() );
                        }
                    }
                    else if( unit.is_number() ) {
                        res.push_back( unit.get<int>() );
                    }
                }
            }
        }
        return res;
    }

    bool Service_DETECTOR::SocketIOEventBind( void )
    {
        auto api_handler = network_manager_->GetApiHandler();
        if( !api_handler ){
            MLOG_ERROR("%s(), api_handler == nullptr", __func__);
            return false;
        }

        auto sio_handler = network_manager_->GetSioHandler();
        if( !sio_handler ){
            MLOG_ERROR("%s(), sio_handler == nullptr", __func__);
            return false;
        }

        auto sm = MGEN::GetSettingManager();

        // ExceptionUpdate
        // ====================================================================================================
        // NEW-10: make_shared 는 throw 또는 non-null 반환. nullptr 검사 dead code 제거.
        auto event_binder_exception_update = std::make_shared<SioEventBinder>
        (
            SocketIO::EventName::DETECTOR::ExceptionUpdate,
            SocketIO::EventType::InterprotocolSettingUpdateTargetUnit
        );

        // Interprotocol(SocketIO 등 통신 중에 다른 통신 Protocol 이용해서 데이터를 받와야 할 때)
        event_binder_exception_update->SetInterProtocolFunc(
            api_handler->BuildCallback_GET(
                // SIO를 통해서 API에 전달할 요청 정보를 담은 JSON 값이 유효한지
                [] ( const nlohmann::json& json ) {
                    const bool success = CheckInterProcJsonValid( json, "camera_id" );
                    if( !success ) {
                        MLOG_ERROR("ExceptionUpdate::CheckInterProcJsonValid( camera_id ) Failed:\n%s",
                        json.dump(2).c_str() );
                    }
                    return success;
                },
                // 요청 정보를 담은 JSON를 이용해서 실제 URL을 만들어 반환하는 람다식
                []( const nlohmann::json& json ){
                    std::string url = GetAPICommandFromJson( json );
                    MLOG_INFO("ExceptionUpdate::GetAPICommandFromJson() => %s", url.c_str() );
                    return url;
                }
            )
        );

        event_binder_exception_update->SetTargetUnitsExtractor(
            // SIO를 통해서 받은 요청 정보를 담은 JSON에서 UnitID 추출
            // 해당 unit에 해당하는 SettingData 들을 Update() 할 예정
            [] ( const nlohmann::json& json ){
                auto units = ExtractUnitFromJson( json, "camera_id" );
                MLOG_INFO("ExceptionUpdate::ExtractUnitFromJson( camera_id ):");
                for( const auto unit : units ) {
                    MLOG_INFO("  * %d", unit);
                }
                return units;
            }
        );

        // Update() 할 SettingData 들을 관리하는 세팅 매니저 등록
        event_binder_exception_update->SetMultiUintsSettingManager( sm->GetExcludeCamSettingsManager() );

        if( event_binder_exception_update->IsRunnable( true ) == true ){
            sio_handler->RegistEvent( event_binder_exception_update );
        }

        // ScheduleUpdate
        // ====================================================================================================
        // NEW-10: make_shared 는 throw 또는 non-null 반환. nullptr 검사 dead code 제거.
        auto event_binder_schedule_update = std::make_shared<SioEventBinder>
        (
            SocketIO::EventName::DETECTOR::ScheduleUpdate,
            SocketIO::EventType::InterprotocolSettingUpdateTargetUnit
        );

        // Interprotocol(SocketIO 등 통신 중에 다른 통신 Protocol 이용해서 데이터를 받와야 할 때)
        event_binder_schedule_update->SetInterProtocolFunc(
            api_handler->BuildCallback_GET(
                // SIO를 통해서 API에 전달할 요청 정보를 담은 JSON 값이 유효한지
                [] ( const nlohmann::json& json ) {
                    const bool success = CheckInterProcJsonValid( json, "camera_id" );
                    if( !success ) {
                        MLOG_ERROR("ScheduleUpdate::CheckInterProcJsonValid( camera_id ) Failed:\n%s",
                        json.dump(2).c_str() );
                    }
                    return success;
                },
                // 요청 정보를 담은 JSON를 이용해서 실제 URL을 만들어 반환하는 람다식
                []( const nlohmann::json& json ){
                    std::string url = GetAPICommandFromJson( json );
                    MLOG_INFO("ScheduleUpdate::GetAPICommandFromJson() => %s", url.c_str() );
                    return url;
                }
            )
        );

        event_binder_schedule_update->SetTargetUnitsExtractor(
            // SIO를 통해서 받은 요청 정보를 담은 JSON에서 UnitID 추출
            // 해당 unit에 해당하는 SettingData 들을 Update() 할 예정
            [] ( const nlohmann::json& json ){
                auto units = ExtractUnitFromJson( json, "camera_id" );
                MLOG_INFO("ScheduleUpdate::ExtractUnitFromJson( camera_id ):");
                for( const auto unit : units ) {
                    MLOG_INFO("  * %d", unit);
                }
                return units;
            }
        );

        // Update() 할 SettingData 들을 관리하는 세팅 매니저 등록
        event_binder_schedule_update->SetMultiUintsSettingManager( sm->GetScheduleSettingsManager() );

        if( event_binder_schedule_update->IsRunnable( true ) == true ){
            sio_handler->RegistEvent( event_binder_schedule_update );
        }

        return true;
    }

}