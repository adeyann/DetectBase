#pragma once

#include "SetterBase.h"         // Include SetterBase for callback types & manager alias
#include "SettingManagerBase.h" // Include SettingManagerBase for manager alias
#include "SettingData.h"        // Include specific setting data types

#include "ServiceProfile.h"  // Domain specific types
#include "json/json_fwd.hpp" // Forward declaration for nlohmann::json
#include "ApiHandler.h"      // For initialization data

#include "ServiceBlockProfile.h"

#include <memory>       // For std::shared_ptr, std::make_shared
#include <vector>
#include <set>
#include <unordered_map>
#include <string>
#include <functional>   // For std::function (callbacks)
#include <mutex>        // For std::once_flag
#include <optional>     // For getters returning optional data
#include <atomic>

namespace MGEN
{
    // Define constants and enums
    DEFINE SERVER_SERVICE_ID_NOT_SET = -1;

    // Structure for initialization data
    struct SettingInitData
    {
        std::shared_ptr<ApiHandler> api_handler;
        std::string                 service_tag;
        nlohmann::json              extend_data;
    };

    // --- Type Aliases for Callback Functions ---
    // Defined using the nested type within SetterBase for clarity
    using ExcludeCamSettingUpdateCallback       = typename SetterBase<ExcludeCamSettingData>::SettingUpdateCallback;
    using ScheduleSettingUpdateCallback         = typename SetterBase<ScheduleSettingData>::SettingUpdateCallback;

    // --- Type Aliases for Managers ---
    template <typename T> using UniquenessSettingManager = SetterBase<T>;
    template <typename T> using MultiUintsSettingManager = SettingManagerBase<T>;

    // --- Domain Type Alias ---
    // MVAS 등록 카메라 ID set. 이전 DeviceCluster.h (CameraCluster_DETECTOR 폐기, 2026-05-27 SettingManager 흡수).
    using CameraIDSet = std::set<MGEN::Type::DeviceID>;

    // --- Main SettingManager Facade Class ---
    class SettingManager final // Mark final as it's a singleton facade
    {
    // --- Private Section ---
    private:
        // Constructor private for singleton pattern
        SettingManager( void ) noexcept;

        // Prevent copying and assignment for Singleton
        SettingManager(const SettingManager&) = delete;
        SettingManager& operator=(const SettingManager&) = delete;
        SettingManager(SettingManager&&) = delete;
        SettingManager& operator=(SettingManager&&) = delete;

        // Internal initialize implementations (DETECTOR 단일 분기)
        bool InitializeImpl_DETECTOR_BaseForm( const SettingInitData& init_data, const MGEN::API::URL server_setting_req_url_type );
        bool InitializeImpl_DETECTOR         ( const SettingInitData& init_data );

        // Internal setters for cached camera info (called during Initialize)
        bool SetCameraCluster_DETECTOR( const SettingInitData& init_data );

        // Internal logging helper
        void ShowInitializeStatus( std::string_view service_tag );

        // Internal setters for setting manager instances (called during Initialize)
        void SetServerSetting           ( const nlohmann::json& init_json );
        void SetCameraSettings          ( const nlohmann::json& init_json );
        void SetExcludeCamSettings      ( const nlohmann::json& init_json );
        void SetScheduleSettings        ( const nlohmann::json& init_json );

    // --- Public Section ---
    public:
        // Singleton Access Method
        static std::shared_ptr<SettingManager> GetSingletonSettingManager( void );

        // Initialization Method (intended to be called once)
        bool Initialize( const SettingInitData&& init_data );

        // --- Getters for underlying Setting Managers ---
        std::shared_ptr<MultiUintsSettingManager<CameraSettingData>>           GetCameraSettingsManager          ( void ) const noexcept;
        std::shared_ptr<MultiUintsSettingManager<ExcludeCamSettingData>>       GetExcludeCamSettingsManager      ( void ) const noexcept;
        std::shared_ptr<MultiUintsSettingManager<ScheduleSettingData>>         GetScheduleSettingsManager        ( void ) const noexcept;

        // --- Getters for specific setting data ---
        std::optional<ServerSettingData>           GetServerSetting           ( void ) const noexcept;
        std::optional<CameraSettingData>           GetCameraSetting           ( const UnitID id ) const noexcept;
        std::optional<ExcludeCamSettingData>       GetExcludeCamSetting       ( const UnitID id ) const noexcept;
        std::optional<ScheduleSettingData>         GetScheduleSetting         ( const UnitID id ) const noexcept;

        // --- Getters for Cached Camera Info / Server ID ---
        int                GetServerServiceID( void )    const noexcept;
        CameraIDSet        GetCameraIDSet( void )        const noexcept;

        // --- Callback Registration/Unregistration Methods ---

        // Exclude Camera Setting Callbacks (Multi-Unit)
        UUIDType RegisterExcludeCamSettingCallback  ( UnitID camera_id, ExcludeCamSettingUpdateCallback callback );
        bool     UnregisterExcludeCamSettingCallback( UnitID camera_id, const UUIDType& uuid );

        // Schedule Setting Callbacks (Multi-Unit)
        UUIDType RegisterScheduleSettingCallback  ( UnitID camera_id, ScheduleSettingUpdateCallback callback );
        bool     UnregisterScheduleSettingCallback( UnitID camera_id, const UUIDType& uuid );

    // --- Private Members ---
    private:
        // Setting Manager Instances (using shared_ptr for lifetime management)
        std::shared_ptr<UniquenessSettingManager<ServerSettingData>>           server_setting;
        std::shared_ptr<MultiUintsSettingManager<CameraSettingData>>           camera_settings;
        std::shared_ptr<MultiUintsSettingManager<ExcludeCamSettingData>>       exclude_cam_settings;
        std::shared_ptr<MultiUintsSettingManager<ScheduleSettingData>>         schedule_settings;

        // Cached Info (updated during initialization)
        // MVAS REST `/cluster` 응답에서 추출된 등록 카메라 ID set.
        // SetCameraCluster_DETECTOR() 가 직접 채움 (이전 CameraCluster_DETECTOR 폐기, 2026-05-27).
        CameraIDSet camera_id_set_;

        int server_service_id;

        std::atomic<bool>  initialization_succeeded { false };
        mutable std::mutex initialize_mutex;
    };

    // --- Singleton Access Function (이전 GetSettingManager 매크로 대체) ---
    inline std::shared_ptr<SettingManager> GetSettingManager()
    {
        return SettingManager::GetSingletonSettingManager();
    }

} // namespace MGEN
