#include "SettingManager.h"
#include "ServiceProfile.h" // Include domain types if needed by Impl functions
#include "MgenLogger.h"
#include "ApiHandler.h"     // Include ApiHandler for Impl functions
#include "UUIDGenerator.h"  // Include UUID (though generation is in SetterBase)

#include <vector>
#include <mutex> // Added for call_once
#include <thread> // Added for call_once (though std::call_once includes necessary headers)
#include <set>    // Include set for CameraSet if used in Impl functions

namespace MGEN
{
    namespace // Use anonymous namespace for internal linkage helpers
    {
        static std::string GetSearchServerIP( const nlohmann::json& server_setting_json_array, const std::shared_ptr<ApiHandler>& api_handler )
        {
            std::string search_server_ip { "" };
            search_server_ip.reserve( 16 );

            if( server_setting_json_array.is_array() == false || server_setting_json_array.empty() )
                return search_server_ip; // ""

            const auto& object = server_setting_json_array.front();
            if( object.contains( "Setting" ) == false )
                return search_server_ip; // ""

            int search_server_id = object["Setting"].value( "SearchServerId", 0 );
            if( search_server_id == 0 )
                return search_server_ip; // ""

            // Ensure MGEN::API::URL is accessible or qualify it
            const auto search_req = MGEN::API::MakeURL( MGEN::API::URL::API_GET_SERVER_BY_ID, search_server_id );
            const auto [ search_rsp_code, search_rsp_body ] = api_handler->GET_or_throw_if_timeout( search_req );
            if( !ApiHandler::IsGoodResponse( search_rsp_code ) ) {
                MLOG_ERROR( "GetSearchServerIP(), REST API GET ( %s ) -> %d Code",
                    search_req.c_str(), search_rsp_code );
                return search_server_ip;
            }
            if( !search_rsp_body.is_array() || search_rsp_body.empty() || !search_rsp_body.front().contains("Ip") ) {
                MLOG_ERROR( "GetSearchServerIP(), REST API GET ( %s ) -> Unexpected body\n%s",
                    search_req.c_str(), search_rsp_body.dump(2).c_str() );
                return search_server_ip;
            }
            search_server_ip = search_rsp_body.front().value( "Ip", "" );
            return search_server_ip;
        }

    } // end anonymous namespace

    // --- InitializeImpl functions ---
    // These perform the actual API calls and data processing during initialization.
    // They call the internal Set... methods. Ensure error handling is robust.
    // (Definitions omitted for brevity - assume they exist and function correctly,
    // calling methods like SetServerSetting, SetCameraSet, SetCameraSettings etc.)
    bool SettingManager::InitializeImpl_DETECTOR_BaseForm( const SettingInitData& init_data, const MGEN::API::URL server_setting_req_url_type )
    {
        const auto& api_handler = init_data.api_handler;
        if (!api_handler) { MLOG_ERROR("InitializeImpl_DETECTOR_BaseForm: API Handler is null."); return false; }

        // Get Server ID
        const auto my_local_ip = api_handler->GetMyLocalIp();
        const auto server_req = MGEN::API::MakeURL( server_setting_req_url_type, my_local_ip );
        const auto [ server_rsp_code, server_rsp_body ] = api_handler->GET_or_throw_if_timeout( server_req );
        if( !ApiHandler::IsGoodResponse( server_rsp_code ) ) {
            MLOG_ERROR( "Initialize: REST API GET ( %s ) -> %d Code", server_req.c_str(), server_rsp_code );
            return false;
        }
        if( !server_rsp_body.is_array() || server_rsp_body.empty() || !server_rsp_body.front().is_object() || !server_rsp_body.front().contains("Id") ) {
            MLOG_ERROR( "Initialize: REST API GET ( %s ) -> Invalid response body\n%s", server_req.c_str(), server_rsp_body.dump(2).c_str() );
            return false;
        }
        this->server_service_id = server_rsp_body.front().value( "Id", SERVER_SERVICE_ID_NOT_SET );
        if( this->server_service_id == SERVER_SERVICE_ID_NOT_SET ){
            MLOG_ERROR("Initialize: Failed to get valid Server Service ID from response.");
            return false;
        }

        // Set Server Setting (including potential Search Server IP lookup)
        nlohmann::json server_setter_js = server_rsp_body; // Copy response
        const auto search_server_ip = GetSearchServerIP( server_setter_js, api_handler );
        if( !search_server_ip.empty() ) {
            (server_setter_js.front())["Setting"]["SearchServerIp"] = search_server_ip;
        }
        this->SetServerSetting( server_setter_js ); // Initialize SetterBase<ServerSettingData>

        // Get Camera List via Cluster
        if( this->SetCameraCluster_DETECTOR( init_data ) == false ) {
            MLOG_ERROR("Initialize: Failed to SetCameraCluster_DETECTOR()");
            return false;
        }

        auto cams = this->camera_id_set_;

        // Get Camera Settings
        if( !cams.empty() ) {
            const auto camera_req = MGEN::API::MakeURL( MGEN::API::URL::API_GET_CAMERA, cams );
            const auto [ camera_rsp_code, camera_rsp_body ] = api_handler->GET_or_throw_if_timeout( camera_req );
            if( !ApiHandler::IsGoodResponse( camera_rsp_code ) ) {
                MLOG_ERROR( "Initialize: REST API GET ( %s ) -> %d Code", camera_req.c_str(), camera_rsp_code );
                return false;
            }
            // Camera settings might be empty array, which is valid if no settings exist
            if( !camera_rsp_body.is_array() ) {
                MLOG_ERROR( "Initialize: REST API GET ( %s ) -> Invalid response body (not array)\n%s", camera_req.c_str(), camera_rsp_body.dump(2).c_str() );
                return false; // Expect array even if empty
            }
            this->SetCameraSettings( camera_rsp_body ); // Initialize SettingManagerBase<CameraSettingData>
        } else {
            MLOG_INFO("Initialize: No cameras in cluster, setting empty camera settings.");
            this->SetCameraSettings( nlohmann::json::array() ); // Set empty array
        }

        // Get Exclude Camera Settings
        const auto exclude_req = MGEN::API::MakeURL( API::URL::API_GET_EXCEPTION_CAMERA, cams ); // Pass cams even if empty? API should handle.
        const auto [ exclude_rsp_code, exclude_rsp_body ] = api_handler->GET_or_throw_if_timeout( exclude_req );
        if( !ApiHandler::IsGoodResponse( exclude_rsp_code ) ) {
            MLOG_ERROR( "Initialize: REST API GET ( %s ) -> %d Code", exclude_req.c_str(), exclude_rsp_code );
            return false;
        }
        if( !exclude_rsp_body.is_array() ) {
            MLOG_ERROR( "Initialize: REST API GET ( %s ) -> Invalid response body (not array)\n%s", exclude_req.c_str(), exclude_rsp_body.dump(2).c_str() );
            return false;
        }
        this->SetExcludeCamSettings( exclude_rsp_body );

        // Get Schedule Settings
        const auto schedule_req = MGEN::API::MakeURL( API::URL::API_GET_SCHEDULE, cams ); // Pass cams even if empty?
        const auto [ schedule_rsp_code, schedule_rsp_body ] = api_handler->GET_or_throw_if_timeout( schedule_req );
        if( !ApiHandler::IsGoodResponse( schedule_rsp_code ) ) {
            MLOG_ERROR( "Initialize: REST API GET ( %s ) -> %d Code", schedule_req.c_str(), schedule_rsp_code );
            return false;
        }
        if( !schedule_rsp_body.is_array() ) {
            MLOG_ERROR( "Initialize: REST API GET ( %s ) -> Invalid response body (not array)\n%s", schedule_req.c_str(), schedule_rsp_body.dump(2).c_str() );
            return false;
        }
        this->SetScheduleSettings( schedule_rsp_body );

        return true;
    }

    bool SettingManager::SetCameraCluster_DETECTOR( const SettingInitData& init_data )
    {
        if( this->GetServerServiceID() == SERVER_SERVICE_ID_NOT_SET ){
            MLOG_ERROR("SetCameraCluster_DETECTOR(), Failed to get valid Server Service ID (%d)",
                this->GetServerServiceID() );
            return false;
        }

        const auto& api_handler = init_data.api_handler;
        if( !api_handler ) {
            MLOG_ERROR("SetCameraCluster_DETECTOR(), API Handler is null.");
            return false;
        }

        const auto cluster_req = MGEN::API::MakeURL( MGEN::API::URL::API_GET_CLUSTER, this->GetServerServiceID() );
        const auto [ cluster_rsp_code, cluster_rsp_body ] = api_handler->GET_or_throw_if_timeout( cluster_req );
        if( !ApiHandler::IsGoodResponse( cluster_rsp_code ) ) {
            MLOG_ERROR( "SetCameraCluster_DETECTOR(), REST API GET ( %s ) -> %d Code",
                cluster_req.c_str(), cluster_rsp_code );
            return false;
        }
        if( !cluster_rsp_body.is_array() ) {
            MLOG_ERROR( "SetCameraCluster_DETECTOR(), REST API GET ( %s ) -> Invalid response body (not array)\n%s",
                cluster_req.c_str(), cluster_rsp_body.dump(2).c_str() );
            return false;
        }

        // 이전 CameraCluster_DETECTOR::AddClusterData() 의 parse 로직을 직접 인라인 (2026-05-27 폐기).
        constexpr int JSON_PARSE_ID_VALUE_NOT_SET = -1;
        this->camera_id_set_.clear();
        for( const auto& object : cluster_rsp_body ) {
            if( object.is_object() && object.contains( "CameraId" ) ) {
                const auto cam_id = object.value( "CameraId", JSON_PARSE_ID_VALUE_NOT_SET );
                if( cam_id != JSON_PARSE_ID_VALUE_NOT_SET ) {
                    this->camera_id_set_.insert( cam_id );
                }
            }
        }
        return true;
    }

    bool SettingManager::InitializeImpl_DETECTOR( const SettingInitData& init_data )
    {
        return this->InitializeImpl_DETECTOR_BaseForm( init_data, MGEN::API::URL::API_GET_SERVER_BY_IP );
    }

    // --- Constructor and Singleton ---
    SettingManager::SettingManager( void ) noexcept
        : server_setting          ( nullptr )
        , camera_settings         ( nullptr )
        , exclude_cam_settings    ( nullptr )
        , schedule_settings       ( nullptr )
        , server_service_id       ( SERVER_SERVICE_ID_NOT_SET )
    {
        // Constructor now only initializes members to default state
    }

    // Static instance for Singleton pattern
    std::shared_ptr<SettingManager> SettingManager::GetSingletonSettingManager( void )
    {
        // Meyers' Singleton - thread-safe initialization in C++11+
        static std::shared_ptr<SettingManager> singleton ( new SettingManager() );
        return singleton;
    }

    bool SettingManager::Initialize( const SettingInitData&& init_data )
    {
        if( initialization_succeeded.load() == true )
            return true;

        std::string error_message {}; // Capture error message
        error_message.reserve(64);

        try {
            std::unique_lock<std::mutex> init_lock { this->initialize_mutex };

            // double-check: 다른 thread 가 락 획득 직전 init 완료했을 가능성
            if( initialization_succeeded.load() == true )
                return true;

            // Local variable to capture success within the lambda for *this specific call*
            bool current_call_success = false;

            if( init_data.service_tag == std::string { EntireServiceTag::DETECTOR } )
            {
                current_call_success = InitializeImpl_DETECTOR( init_data );
                if( !current_call_success )
					error_message = "InitializeImpl_DETECTOR failed.";
            }
            else
            {
                current_call_success = false;
                error_message = "No matching service type found for initialization.";
            }

            if( current_call_success == true ){
                this->ShowInitializeStatus( init_data.service_tag );
                initialization_succeeded.store(true);
            }
            else {
                MLOG_ERROR("SettingManager::Initialize() first call failed: %s", error_message.c_str());
                initialization_succeeded.store(false);
            }

            init_lock.unlock();

            // Return the global success status.
            if( initialization_succeeded.load() == false ){
                MLOG_ERROR("SettingManager::Initialize() called, but initialization failed previously or in this call.");
            }
            return initialization_succeeded.load();
        }
        catch( const std::runtime_error& e ) {
            MLOG_ERROR("SettingManager::Initialize() exception: API runtime error: %s", e.what() );
            initialization_succeeded.store(false);
            return false;
        }
        catch( const std::exception& e ) {
            MLOG_ERROR("SettingManager::Initialize() exception: Undefined error: %s", e.what() );
            initialization_succeeded.store(false);
            return false;
        }
        catch( ... ) {
            MLOG_ERROR("SettingManager::Initialize() exception: Unknown error." );
            initialization_succeeded.store(false);
            return false;
        }
        return false;
    }

    // --- Log Status ---
    void SettingManager::ShowInitializeStatus( [[maybe_unused]] std::string_view service_tag )
    {
        MLOG_DEBUG( "------------------------------------------" );
        MLOG_DEBUG( "   SettingManager Initialization Status   " );
        MLOG_DEBUG( "------------------------------------------" );

		const std::string service_id_str
			= ( this->server_service_id != SERVER_SERVICE_ID_NOT_SET )
			? std::to_string( this->server_service_id )
			: "NOT_SET";

        MLOG_INFO("   - Settings Loaded Using API : Service ID [ %s ] Camera Count [ %zu ]",
            service_id_str.c_str(),
            this->GetCameraIDSet().size()
        );
    }

    // --- Internal Setters (Called by InitializeImpl_...) ---
    void SettingManager::SetServerSetting( const nlohmann::json& init_json )
    {
        // Server setting likely doesn't change dynamically after init,
        // but callbacks might be added/removed, so lock might be useful for callback map.
        const bool use_data_lock = false; // Assume data is read-only after init
        this->server_setting = std::make_shared<UniquenessSettingManager<ServerSettingData>>( init_json, use_data_lock );
        // Initial update happens in SetterBase constructor, triggering callbacks if any were somehow registered before return.
    }

    void SettingManager::SetCameraSettings( const nlohmann::json& init_json )
    {
        const std::string unit_key_name = "CameraId";
        // Use lock for data if individual camera settings can be updated dynamically later.
        // Lock is definitely needed for the map in SettingManagerBase.
        const bool use_setter_data_lock = true;
        this->camera_settings = std::make_shared<MultiUintsSettingManager<CameraSettingData>>(
            init_json, unit_key_name, use_setter_data_lock
        );
    }

    void SettingManager::SetExcludeCamSettings( const nlohmann::json& init_json )
    {
        const std::string unit_key_name = "CameraId";
        const bool use_setter_data_lock = true;
        this->exclude_cam_settings = std::make_shared<MultiUintsSettingManager<ExcludeCamSettingData>>(
            init_json, unit_key_name, use_setter_data_lock
        );
    }

    void SettingManager::SetScheduleSettings( const nlohmann::json& init_json )
    {
        const std::string unit_key_name = "CameraId";
        const bool use_setter_data_lock = true;
        this->schedule_settings = std::make_shared<MultiUintsSettingManager<ScheduleSettingData>>(
            init_json, unit_key_name, use_setter_data_lock
        );
    }


    // --- Public Getters for Setting Managers ---
    std::shared_ptr<MultiUintsSettingManager<CameraSettingData>>
    SettingManager::GetCameraSettingsManager( void ) const noexcept
    {
        return this->camera_settings;
    }

    std::shared_ptr<MultiUintsSettingManager<ExcludeCamSettingData>>
    SettingManager::GetExcludeCamSettingsManager( void ) const noexcept
    {
        return this->exclude_cam_settings;
    }

    std::shared_ptr<MultiUintsSettingManager<ScheduleSettingData>>
    SettingManager::GetScheduleSettingsManager( void ) const noexcept
    {
        return this->schedule_settings;
    }

    std::optional<ServerSettingData>
    SettingManager::GetServerSetting( void ) const noexcept
    {
        // Check if manager exists before calling methods on it
        if( this->server_setting == nullptr ) {
            // Log only if initialization should have happened but failed
            // MLOG_WARN("GetServerSetting called before successful initialization.");
            return std::nullopt;
        }
        return this->server_setting->GetSetting();
    }

    std::optional<CameraSettingData>
    SettingManager::GetCameraSetting( const UnitID id ) const noexcept
    {
        if( this->camera_settings == nullptr ) return std::nullopt;
        return this->camera_settings->GetSetting( id );
    }

    std::optional<ExcludeCamSettingData>
    SettingManager::GetExcludeCamSetting( const UnitID id ) const noexcept
    {
        if( this->exclude_cam_settings == nullptr ) return std::nullopt;
        return this->exclude_cam_settings->GetSetting( id );
    }

    std::optional<ScheduleSettingData>
    SettingManager::GetScheduleSetting( const UnitID id ) const noexcept
    {
        if( this->schedule_settings == nullptr ) return std::nullopt;
        return this->schedule_settings->GetSetting( id );
    }

    // --- Public Getters for Camera Info / Server ID ---
    CameraIDSet SettingManager::GetCameraIDSet( void ) const noexcept
    {
        return this->camera_id_set_;
    }

    int SettingManager::GetServerServiceID( void ) const noexcept
    {
        return this->server_service_id;
    }

    // --- Callback Registration/Unregistration Implementation ---

    // Exclude Camera Setting Callbacks
    UUIDType SettingManager::RegisterExcludeCamSettingCallback( UnitID camera_id, ExcludeCamSettingUpdateCallback callback )
    {
        if( this->exclude_cam_settings != nullptr ){
            return this->exclude_cam_settings->RegisterCallback( camera_id, std::move(callback) );
        }
        MLOG_WARN("RegisterExcludeCamSettingCallback: SettingManager not initialized or exclude_cam_settings is null.");
        return UUID::Empty();
    }

    bool SettingManager::UnregisterExcludeCamSettingCallback( UnitID camera_id, const UUIDType& uuid )
    {
        if( this->exclude_cam_settings != nullptr ){
            return this->exclude_cam_settings->UnregisterCallback( camera_id, uuid );
        }
        MLOG_WARN("UnregisterExcludeCamSettingCallback: SettingManager not initialized or exclude_cam_settings is null.");
        return false;
    }

    // Schedule Setting Callbacks
    UUIDType SettingManager::RegisterScheduleSettingCallback( UnitID camera_id, ScheduleSettingUpdateCallback callback )
    {
        if( this->schedule_settings != nullptr ){
            return this->schedule_settings->RegisterCallback( camera_id, std::move(callback) );
        }
        MLOG_WARN("RegisterScheduleSettingCallback: SettingManager not initialized or schedule_settings is null.");
        return UUID::Empty();
    }

    bool SettingManager::UnregisterScheduleSettingCallback( UnitID camera_id, const UUIDType& uuid )
    {
        if( this->schedule_settings != nullptr ){
            return this->schedule_settings->UnregisterCallback( camera_id, uuid );
        }
        MLOG_WARN("UnregisterScheduleSettingCallback: SettingManager not initialized or schedule_settings is null.");
        return false;
    }

} // namespace MGEN
