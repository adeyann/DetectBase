#pragma once

#include "ISettingManager.h"
#include "SetterBase.h"      // Include SetterBase for T and its methods
#include "MgenTypes.h"
#include "MgenLogger.h"      // Include MgenLogger
#include "MetricsRegistry.h" // P54: setting_partial_failure_total
#include "json/json.hpp"
#include "UUIDGenerator.h"

#include <unordered_map>
#include <memory>       // For std::shared_ptr, std::make_shared
#include <algorithm>
#include <set>           // Include set for GetRegistedUnitList
#include <string>        // Include string for UUID
#include <optional>      // Include optional for GetSetting
#include <mutex>         // Include mutex for map protection
#include <utility>       // For std::move

namespace MGEN
{
    constexpr UnitID MISSING_UNIT_ID = -99;

    template <typename T>
    class SettingManagerBase : public ISettingManager // Public inheritance
    {
        // concept
        static_assert( std::is_base_of<ISettingData, T>::value, "Template T must derive from ISettingData" );

    public:
        // Define callback type by forwarding from SetterBase<T>
        // This callback receives only the data, UnitID context is provided during registration.
        using SettingUpdateCallback = typename SetterBase<T>::SettingUpdateCallback;

        // constructor
        // need_setter_data_lock controls the lock within each *individual* SetterBase instance it creates.
        explicit SettingManagerBase( std::string_view unit_key_name, const bool need_setter_data_lock = false )
            : ISettingManager( unit_key_name )
            , need_setter_data_lock_( need_setter_data_lock ) // Store for creating new Setters
        {
            //
        }

        // Constructor initializing from JSON
        explicit SettingManagerBase( const nlohmann::json& js, std::string_view unit_key_name, const bool need_setter_data_lock = false )
            : ISettingManager( unit_key_name )
            , need_setter_data_lock_( need_setter_data_lock )
        {
            // Use RenewAfterReset for initial population for atomicity and callback invocation
            if( !this->RenewAfterReset( js ) ){
                MLOG_ERROR("SettingManagerBase initial population via RenewAfterReset failed for key '%s'.", unit_key_name.data());
                // Consider throwing an exception or setting an error state
            }
        }

        // parent class destructor must virtual
        virtual ~SettingManagerBase() = default;

        // Prevent copying and assignment
        SettingManagerBase(const SettingManagerBase&) = delete;
        SettingManagerBase& operator=(const SettingManagerBase&) = delete;
        SettingManagerBase(SettingManagerBase&&) = delete;
        SettingManagerBase& operator=(SettingManagerBase&&) = delete;

        // --- Basic Getters ---
        bool IsUnitExist( const UnitID id ) const noexcept
        {
            std::lock_guard<std::mutex> lck { this->map_mutex_ };
            return ( settings_.count( id ) > 0 );
        }

        virtual std::set<UnitID> GetRegistedUnitList( void ) const override final
        {
            std::set<UnitID> keys;
            std::lock_guard<std::mutex> lck { this->map_mutex_ };
            for( const auto& [key, _] : this->settings_ ) {
                keys.insert( key );
            }
            return keys;
        }

        // Get setting data for a specific unit
        std::optional<T> GetSetting( const UnitID id ) noexcept
        {
            std::shared_ptr<SetterBase<T>> setter = nullptr;
            {   // Lock scope for map access
                std::lock_guard<std::mutex> lck { this->map_mutex_ };

                auto it = settings_.find( id );
                if( it == settings_.end() ){ return std::nullopt; }

                // Copy shared_ptr while holding lock
                // The SetterBase object itself is still managed by the map's shared_ptr
                setter = it->second;
            } // Release map lock before calling setter method

            // Delegate to the specific SetterBase instance
            // GetSetting in SetterBase handles its own internal locking if needed
            if( setter ){
                return setter->GetSetting();
            } else {
                // This path should ideally not be reached if found in map
                MLOG_ERROR("SettingManagerBase::GetSetting inconsistency: UnitID %d found but setter pointer is null.", id);
                return std::nullopt;
            }
        }

        // Helper to extract UnitID from JSON (no change)
        UnitID GetUnitID( const nlohmann::json& json_object ) const
        {
            if( this->GetUnitKeyName().empty() )                  return MISSING_UNIT_ID;
            if( !json_object.is_object() )                        return MISSING_UNIT_ID;
            if( !json_object.contains( this->GetUnitKeyName() ) ) return MISSING_UNIT_ID;

            // Provide default value just in case .value() is used on non-number type
            return json_object.value( this->GetUnitKeyName(), MISSING_UNIT_ID );
        }

        // --- Update/Remove Operations ---

        // Update or insert a setting for a specific unit.
        // Returns true if update/insert was successful, false otherwise.
        // Callbacks are invoked internally by the SetterBase upon successful update.
        virtual bool UpdateTargetUnit( const UnitID id, const nlohmann::json& update_json ) override final
        {
            if( id == MISSING_UNIT_ID ){
                MLOG_ERROR("UpdateTargetUnit called with MISSING_UNIT_ID.");
                return false;
            }

            std::shared_ptr<SetterBase<T>> setter = nullptr;
            // --- Find or Create Setter (Protected by map_mutex_) ---
            {
                std::lock_guard<std::mutex> lck { this->map_mutex_ };
                auto it = settings_.find( id );
                if( it != settings_.end() ){
                    setter = it->second; // Found existing
                }
                else {
                    // Create new one with the stored lock preference
                    setter = std::make_shared<SetterBase<T>>( this->need_setter_data_lock_ );
                    settings_[id] = setter;
                }
            } // Release map lock before calling Update

            // --- Perform Update via SetterBase ---
            if( !setter ){
                // Should not happen due to map insertion logic
                MLOG_ERROR("SettingManagerBase::UpdateTargetUnit critical error: Failed to get/create setter pointer for ID %d", id);
                return false;
            }

            // Delegate update; SetterBase::Update handles its internal data locking
            // and invokes callbacks on success.
            bool update_success = setter->Update( update_json );

            // Log results
            if( update_success == false) {
                MLOG_WARN("SettingManagerBase::Update() failed for existing ID %d, JSON = %s", id, update_json.dump().c_str());
            }
            return update_success;
        }

        // Remove setting for a specific unit.
        // Returns true if the unit was found and removed, false otherwise.
        virtual bool RemoveTargetUnit( const UnitID id ) override final
        {
            if( id == MISSING_UNIT_ID ){
                MLOG_ERROR("RemoveTargetUnit called with MISSING_UNIT_ID.");
                return false;
            }

            size_t erased_count = 0;
            {   // Lock scope for map modification
                std::lock_guard<std::mutex> lck { this->map_mutex_ };
                erased_count = settings_.erase( id );
            }
            return ( erased_count > 0 );
        }

        // Update multiple units based on a JSON array.
        // Returns true if ALL individual updates were successful, false if ANY failed.
        virtual bool UpdateBulkUnits( const nlohmann::json& update_json ) override final
        {
            if( !update_json.is_array() ){
                MLOG_ERROR("UpdateBulkUnits expects a JSON array. Input: %s", update_json.dump(2).c_str());
                return false;
            }

            // --- Sort data first (no map access needed yet) ---
            std::unordered_map<UnitID, nlohmann::json::array_t> update_data_sort_cluster {};
            for( const auto& object : update_json ) {
                if( !object.is_object() ) {
                    MLOG_WARN("UpdateBulkUnits skipping non-object item in array: %s", object.dump().c_str());
                    continue;
                }
                if( !object.contains( this->GetUnitKeyName() ) ) {
                    MLOG_WARN("UpdateBulkUnits skipping object missing key '%s': %s", this->GetUnitKeyName().c_str(), object.dump().c_str());
                    continue;
                }
                const UnitID unit_id = this->GetUnitID( object );
                if( unit_id == MISSING_UNIT_ID ) {
                    MLOG_WARN("UpdateBulkUnits skipping object with invalid UnitID: %s", object.dump().c_str());
                    continue;
                }
                // Group all json objects intended for the same unit_id
                update_data_sort_cluster[unit_id].push_back( object );
            }

            // --- Perform Updates Unit by Unit ---
            bool all_success = true;
            for( const auto& [ unit_id, update_data_array ] : update_data_sort_cluster ) {
                // Use UpdateTargetUnit which handles creation/update and triggers callbacks internally
                // It uses the full array for settings like ScheduleSettingData
                if( this->UpdateTargetUnit( unit_id , update_data_array ) == false ) {
                    all_success = false;
                    MLOG_ERROR("UpdateBulkUnits: Update failed for UnitID %d.", unit_id);
                }
            }

            if( all_success == false ){
                MLOG_WARN("UpdateBulkUnits completed, but one or more unit updates failed.");
            }
            return all_success;
        }


        // Atomically replace all settings with new ones from the JSON array.
        // Existing units not in renew_json are removed.
        // Callbacks are invoked by the new SetterBase instances upon their successful initialization.
        virtual bool RenewAfterReset( const nlohmann::json& renew_json ) override final
        {
            if( !renew_json.is_array() ){
                MLOG_ERROR("RenewAfterReset expects a JSON array. Input: %s", renew_json.dump(2).c_str());
                return false;
            }

            // --- 임시 저장소 준비 ---
            // 최종적으로 교체될 새로운 설정 맵
            std::unordered_map<UnitID, std::shared_ptr<SetterBase<T>>> map_for_swap;
            // 이전 Setter 에서 임시로 가져온(stolen) 콜백들을 저장할 맵
            std::map<UnitID, std::unordered_map<std::string, SettingUpdateCallback>> stolen_callbacks_map;
            bool preparation_ok = true;

            // --- Phase 1: 새 Setter 생성, 초기화 및 이전 콜백 가져오기 ---
            try { // 예외 발생 가능성 고려 (예: make_shared 실패 등)

                // 입력 데이터 정렬
                std::unordered_map<UnitID, nlohmann::json::array_t> renew_data_sort_cluster {};
                for( const auto& object : renew_json ) {
                    if( !object.is_object() || !object.contains( this->GetUnitKeyName() ) )
                        continue;

                    const UnitID unit_id = this->GetUnitID( object );
                    if( unit_id == MISSING_UNIT_ID )
                        continue;

                    renew_data_sort_cluster[unit_id].push_back( object );
                }

                // 새 Setter 생성 및 초기화, 기존 콜백 훔치기 (기존 맵 읽기 접근 필요)
                std::unique_lock preparation_lock(this->map_mutex_); // 기존 settings_ 읽기 위해 잠금

                for( const auto& [ unit_id, init_data_array ] : renew_data_sort_cluster ){
                    auto new_setter = std::make_shared<SetterBase<T>>( this->need_setter_data_lock_ );

                    // 새 Setter 초기화 시도 (콜백은 아직 호출 안 됨)
                    if( new_setter->Update( init_data_array ) == true ){

                        // 기존 UnitID 가 있었는지 확인하고 콜백 훔치기
                        auto old_it = this->settings_.find( unit_id );
                        if( old_it != this->settings_.end() && old_it->second ){
                            // StealCallbacks 호출 (SetterBase 내부에서 콜백 뮤텍스 잠금)
                            stolen_callbacks_map[unit_id] = old_it->second->StealCallbacks();
                        }

                        // 성공적으로 준비된 새 Setter를 임시 맵에 추가
                        map_for_swap[unit_id] = std::move(new_setter);
                    } else {
                        // graceful degradation: 한 unit 실패해도 다른 unit 은 진행 (운영 가시성 ↑).
                        // 실패한 unit 은 map_for_swap 에 미포함 → swap 후 settings_ 에서 누락.
                        // 운영자는 메트릭 (setting_partial_failure_total) 으로 인지 가능.
                        MLOG_WARN("RenewAfterReset: skip UnitID %d (Update failed). 부분 적용 진행.", unit_id);
                        MGEN::MetricsRegistry::Instance().IncrementCounter(
                            "detectbase_setting_partial_failure_total",
                            { { "unit_key", this->GetUnitKeyName() }, { "unit_id", std::to_string( unit_id ) } } );
                        // continue: 다음 unit 처리
                    }
                }
                // Phase 1 완료 후 메인 맵 락 해제 (롤백 또는 다음 단계 전에)
                preparation_lock.unlock();

            } catch( const std::exception& e ){
                MLOG_ERROR("RenewAfterReset: Exception during preparation phase: %s", e.what());
                preparation_ok = false;
                // 예외 발생 시에도 훔친 콜백 롤백 필요 : 아래 Phase 2 에서 실시
            } catch (...) {
                MLOG_ERROR("RenewAfterReset: Unknown exception during preparation phase.");
                preparation_ok = false;
                 // 예외 발생 시에도 훔친 콜백 롤백 필요
            }

            // --- Phase 2: 준비 실패 시 롤백 (훔친 콜백 되돌리기) ---
            if( !preparation_ok ){

                MLOG_WARN("RenewAfterReset: Rolling back stolen callbacks due to preparation failure.");

                // map_for_swap 에 들어간 (성공했던) new_setter 들은 여기서 소멸됨
                map_for_swap.clear(); // 명시적으로 클리어

                if( !stolen_callbacks_map.empty() ){

                    // 기존 settings_ 맵에 접근하여 콜백 되돌리기 (락 필요)
                    std::unique_lock rollback_lock(this->map_mutex_);

                    for( auto& [unit_id, stolen_callbacks] : stolen_callbacks_map ){
                        auto old_it = this->settings_.find(unit_id);
                        if( old_it != this->settings_.end() && old_it->second ){
                            // SetCallbacks 호출 (SetterBase 내부에서 콜백 뮤텍스 잠금)
                            old_it->second->SetCallbacks(std::move(stolen_callbacks));
                        }
                    }
                    rollback_lock.unlock();

                    stolen_callbacks_map.clear(); // 롤백 후 임시 저장소 비우기
                }
                return false; // 최종적으로 실패 반환
            }

            // --- Phase 3: 준비 성공 시, 새 Setter에 콜백 설정 ---
            // 이 단계에서는 메인 맵 락 불필요 (map_for_swap 에만 접근)
            for( auto& [unit_id, new_setter] : map_for_swap ){
                auto stolen_it = stolen_callbacks_map.find(unit_id);
                if( stolen_it != stolen_callbacks_map.end() ){
                    // SetCallbacks 호출 (SetterBase 내부에서 콜백 뮤텍스 잠금)
                    if( !stolen_it->second.empty() ) { // 이동할 콜백이 있을 때만 호출
                        new_setter->SetCallbacks(std::move(stolen_it->second));
                    }
                }
            }
            stolen_callbacks_map.clear(); // 모든 콜백 이전 완료 후 비우기

            // --- Phase 4: 원자적으로 맵 교체 ---
            std::vector<std::shared_ptr<SetterBase<T>>> old_setters_to_destroy; // 이전 Setter 임시 저장
            {
                std::lock_guard<std::mutex> swap_lock { this->map_mutex_ };
                this->settings_.swap( map_for_swap ); // map_for_swap 이 이제 이전 데이터를 가짐
            } // swap_lock 해제
            // map_for_swap 소멸 -> 이전 Setter 들 소멸

            // *** Phase 5: 새 상태에 대한 콜백 수동 트리거 ***
            // 락 안에서 setter shared_ptr 만 모은 후 락 해제하고 callback 호출.
            // callback 이 SettingManagerBase 다른 메서드를 호출해도 같은 thread 재진입 deadlock 차단.
            std::vector<std::shared_ptr<SetterBase<T>>> setters_to_trigger;
            {
                std::lock_guard<std::mutex> final_lock(this->map_mutex_);
                setters_to_trigger.reserve( this->settings_.size() );
                for( const auto& [unit_id, setter_ptr] : this->settings_ ){
                    if( setter_ptr ){
                        setters_to_trigger.push_back( setter_ptr );
                    }
                }
            } // map_mutex_ 해제 — 이후 callback 호출은 락 외부

            for( auto& setter_ptr : setters_to_trigger ){
                setter_ptr->TriggerCallbacks();
            }

            return true; // 최종 성공
        }

        // --- Callback Management (Delegation to SetterBase) ---

        // Registers a callback for a specific UnitID.
        // Returns UUID string on success, empty string otherwise.
        std::string RegisterCallback( UnitID id, SettingUpdateCallback callback )
        {
            if( id == MISSING_UNIT_ID ){
                MLOG_ERROR("RegisterCallback called with MISSING_UNIT_ID.");
                return UUID::Empty();
            }
            if( !callback ){
                MLOG_WARN("RegisterCallback called with null callback for UnitID %d.", id);
                return UUID::Empty();
            }

            std::shared_ptr<SetterBase<T>> setter = nullptr;
            {   // Lock scope for map access
                std::lock_guard<std::mutex> lck { this->map_mutex_ };
                auto it = settings_.find( id );
                if( it == settings_.end() ){
                    // MLOG_WARN("RegisterCallback: UnitID %d not found, create setting templates", id);
                    settings_[id] = std::make_shared<SetterBase<T>>( this->need_setter_data_lock_ );

                    if( settings_[id] ){
                        setter = settings_[id];
                    }
                    else {
                        MLOG_WARN("RegisterCallback: UnitID %d not found, create setting templates failed", id);
                        return UUID::Empty(); // Unit doesn't exist yet
                    }
                }
                else {
                    setter = it->second; // Get shared_ptr
                }
             } // Release map lock

            if( setter ){
                // Delegate registration to the specific SetterBase
                return setter->RegisterCallback( std::move(callback) );
            } else {
                // Should not happen if found in map
                MLOG_ERROR("RegisterCallback: Found null setter pointer for UnitID %d.", id);
                return UUID::Empty();
            }
        }

        // Unregisters a callback for a specific UnitID using its UUID.
        // Returns true if successful, false otherwise.
        bool UnregisterCallback( UnitID id, const std::string& uuid )
        {
            if( id == MISSING_UNIT_ID ){
                MLOG_ERROR("UnregisterCallback called with MISSING_UNIT_ID.");
                return false;
            }
            if( uuid.empty() ){
                MLOG_WARN("UnregisterCallback called with empty UUID for UnitID %d.", id);
                return false;
            }

            std::shared_ptr<SetterBase<T>> setter = nullptr;
            {  // Lock scope for map access
                std::lock_guard<std::mutex> lck { this->map_mutex_ };
                auto it = settings_.find( id );
                if( it == settings_.end() ) {
                    MLOG_WARN("UnregisterCallback: UnitID %d not found.", id);
                    return false; // Unit doesn't exist
                }
                setter = it->second; // Get shared_ptr
             } // Release map lock

            if( setter ){
                // Delegate unregistration to the specific SetterBase
                return setter->UnregisterCallback( uuid );
            } else {
                MLOG_ERROR("UnregisterCallback: Found null setter pointer for UnitID %d.", id);
                return false;
            }
        }

    private:
        bool need_setter_data_lock_;   // Store preference for creating new Setters
        mutable std::mutex map_mutex_; // Protects the settings_ map structure (add, remove, find, swap)

        // Use shared_ptr for SetterBase to manage lifetime easily, especially with callbacks potentially holding references?
        // Although callbacks currently take const T&, not storing refs to SetterBase. shared_ptr is safe.
        std::unordered_map<UnitID, std::shared_ptr<SetterBase<T>>> settings_;
    };
}