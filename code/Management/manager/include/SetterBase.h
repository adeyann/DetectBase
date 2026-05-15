#pragma once

#include "json/json.hpp"

#include "ISettingData.h"
#include "UUIDGenerator.h" // Include UUID generator
#include "MgenLogger.h"    // For logging errors
#include "MetricsRegistry.h" // F-F2-04: setting callback 실패 메트릭

#include <cassert>
#include <type_traits>
#include <mutex>
#include <optional>
#include <functional>   // For std::function
#include <string>       // For std::string (UUID)
#include <vector>       // For invoking callbacks safely
#include <unordered_map>// For callback map
#include <utility>      // For std::move

namespace MGEN {
    template<typename Data> class SettingManagerBase; // SettingManagerBase 템플릿 전방 선언
}

namespace MGEN
{
    template <typename T>
    class SetterBase
    {
        // concept
        static_assert( std::is_base_of<ISettingData, T>::value, "Template T must derive from ISettingData" );

        // 해당 타입의 SettingManagerBase 만 friend 로 선언
        friend class SettingManagerBase<T>;

    public:
        // Define callback type specific to this Setter's data type T
        // Passes the newly updated data by const reference.
        using SettingUpdateCallback = std::function<void(const T& new_setting_data)>;

        // constructor
        // need_data_update_lock controls locking for the setting_data_ itself.
        explicit SetterBase( const bool need_data_update_lock = false ) noexcept
            : is_initialized_( false )
            , need_data_update_lock_( need_data_update_lock )
        {
            //
        }

        // Constructor that initializes from JSON
        explicit SetterBase( const nlohmann::json& init_json, const bool need_data_update_lock = false )
            : is_initialized_( false )
            , need_data_update_lock_( need_data_update_lock )
        {
            this->Update( init_json );
        }

        // destructor
        ~SetterBase() = default;

        // Prevent copying and assignment
        SetterBase(const SetterBase&) = delete;
        SetterBase& operator=(const SetterBase&) = delete;
        SetterBase(SetterBase&&) = delete;
        SetterBase& operator=(SetterBase&&) = delete;

        // Update function
        // Returns true if update was successful, false otherwise
        // Triggers registered callbacks on successful update.
        // 락 해제 후 callback 호출 — callback 이 GetSetting 호출해도 같은 thread 재진입 deadlock 차단.
        bool Update( const nlohmann::json& update_json )
        {
            bool update_success = false;
            T data_snapshot;

            // Phase 1: 데이터 업데이트 + 스냅샷 캡처 (data_update_mtx_ 안)
            if( this->need_data_update_lock_ ){
                std::lock_guard<std::mutex> data_lck { this->data_update_mtx_ };
                update_success = this->UpdateInternal( update_json );
                if( update_success ) data_snapshot = this->setting_data_;
            }
            else {
                update_success = this->UpdateInternal( update_json );
                if( update_success ) data_snapshot = this->setting_data_;
            }

            if( !update_success ) return false;

            // Phase 2: callback 수집 (callback_mutex_ 안)
            std::vector<SettingUpdateCallback> callbacks_to_invoke;
            {
                std::lock_guard<std::mutex> cb_lck { this->callback_mutex_ };
                callbacks_to_invoke.reserve( this->callbacks_.size() );
                for( const auto& [uuid, cb] : this->callbacks_ ){
                    if( cb ) callbacks_to_invoke.push_back( cb );
                }
            }

            // Phase 3: callback 호출 (모든 락 외부, data_snapshot 사용)
            for( const auto& cb : callbacks_to_invoke ){
                try {
                    cb( data_snapshot );
                } catch( const std::exception& e ){
                    MLOG_ERROR("Exception caught in SettingUpdateCallback: %s", e.what());
                    MGEN::MetricsRegistry::Instance().IncrementCounter(
                        "detectbase_errors_total", { { "type", "setting_callback" } } );
                } catch( ... ){
                    MLOG_ERROR("Unknown exception caught in SettingUpdateCallback.");
                    MGEN::MetricsRegistry::Instance().IncrementCounter(
                        "detectbase_errors_total", { { "type", "setting_callback" } } );
                }
            }
            return true;
        }

        // Getter - simply returns the current data
        // Returns std::nullopt if the setting has never been successfully initialized.
        std::optional<T> GetSetting( void )
        {
            // Lock data access if needed for reading the copy
            if( this->need_data_update_lock_ ){
                std::lock_guard<std::mutex> data_lck { this->data_update_mtx_ };
                if( !this->is_initialized_ ){
                    return std::nullopt;
                }
                // Return a copy while holding the lock
                return this->setting_data_;
            }
            else {
                 if( !this->is_initialized_ ){
                    return std::nullopt;
                }
                // Assume single-threaded read or data is safe for concurrent reads
                return this->setting_data_;
            }
        }

        // --- Callback Management ---

        // Registers a callback function to be invoked when the setting is updated.
        // Returns a unique UUID string identifying the callback.
        // Returns empty string if callback is null or registration fails.
        std::string RegisterCallback( SettingUpdateCallback callback )
        {
            if( !callback ) {
                MLOG_WARN("Attempted to register a nullptr callback.");
                return MGEN::UUID::Empty();
            }

            auto uuid = MGEN::UUID::GetGenerator()->generate();
            if( uuid.empty() ){
                 MLOG_ERROR("Failed to generate UUID for callback registration.");
                 return MGEN::UUID::Empty();
            }

            {   // Lock scope for map modification
                std::lock_guard<std::mutex> cb_lck { this->callback_mutex_ };
                this->callbacks_[uuid] = std::move(callback);
            }
            return uuid;
        }

        // Unregisters a previously registered callback using its UUID.
        // Returns true if the callback was found and removed, false otherwise.
        bool UnregisterCallback( const std::string& uuid )
        {
            if( uuid.empty() ){
                MLOG_WARN("Attempted to unregister callback with empty UUID.");
                return false;
            }

            size_t erased_count = 0;
            {   // Lock scope for map modification
                std::lock_guard<std::mutex> cb_lck { this->callback_mutex_ };
                erased_count = this->callbacks_.erase( uuid );
            }

            return ( erased_count > 0 );
        }

    private:
        /**
         * @brief 내부 콜백 맵의 소유권을 이동시키고 반환합니다.
         * 호출 후 이 SetterBase 인스턴스의 콜백 맵은 비워집니다.
         * @return 이동된 콜백 맵.
         */
        std::unordered_map<std::string, SettingUpdateCallback> StealCallbacks( void )
        {
            std::lock_guard<std::mutex> lock { callback_mutex_ }; // 콜백 맵 접근 보호
            return std::move(callbacks_); // std::move 를 사용하여 소유권 이전
        }

        /**
         * @brief 외부에서 이동(move)시킨 콜백 맵으로 내부 콜백 맵을 설정합니다.
         * @param new_callbacks 이동 시켜올 콜백 맵 (rvalue reference).
         */
        void SetCallbacks( std::unordered_map<std::string, SettingUpdateCallback>&& new_callbacks )
        {
            std::lock_guard<std::mutex> lock { callback_mutex_ }; // 콜백 맵 접근 보호
            callbacks_ = std::move(new_callbacks); // 이동 대입
        }

        /**
         * @brief 현재 등록된 모든 콜백 함수를 현재 설정 데이터로 즉시 호출합니다.
         * 주로 RenewAfterReset 같이 상태가 완전히 교체된 후 알림을 보낼 때 사용됩니다.
         * 설정이 초기화되지 않았다면 호출되지 않습니다.
         */
        void TriggerCallbacks()
        {
            // 설정 데이터가 최소 한 번 이상 성공적으로 로드되었는지 확인
            if( !this->is_initialized_ ){ return; }

            std::vector<SettingUpdateCallback> callbacks_to_invoke;
            T data_snapshot; // 콜백에 전달할 데이터 스냅샷

            // --- 데이터 스냅샷 및 콜백 목록 복사 (락 범위 최소화) ---
            {   // 데이터 접근 락 (필요한 경우)
                std::unique_lock data_lock(this->data_update_mtx_, std::defer_lock);
                if( need_data_update_lock_ ){
                    data_lock.lock();
                }

                // 현재 데이터 복사
                data_snapshot = this->setting_data_;
                // 데이터 락은 여기서 해제됨 (data_lock 소멸) or 명시적 unlock()

                // 콜백 맵 접근 락
                std::lock_guard<std::mutex> cb_lck(this->callback_mutex_);
                if( !callbacks_.empty() ){
                    // 호출할 콜백 함수 객체 복사 (실제 호출은 락 외부에서)
                    callbacks_to_invoke.reserve(callbacks_.size());
                    for( const auto& [uuid, cb] : callbacks_ ){
                        if( cb ){
                            callbacks_to_invoke.push_back(cb);
                        }
                    }
                }
            } // 콜백 락 해제

            // --- 실제 콜백 호출 (락 외부에서) ---
            if( !callbacks_to_invoke.empty() ){
                const T& current_data_ref = data_snapshot; // const 참조로 전달
                for( const auto& cb : callbacks_to_invoke ){
                    try {
                        cb(current_data_ref); // 콜백 호출
                    } catch (const std::exception& e) {
                        MLOG_ERROR("Exception caught during explicit TriggerCallbacks: %s", e.what());
                        MGEN::MetricsRegistry::Instance().IncrementCounter(
                            "detectbase_errors_total", { { "type", "setting_callback" } } );
                    } catch (...) {
                        MLOG_ERROR("Unknown exception caught during explicit TriggerCallbacks.");
                        MGEN::MetricsRegistry::Instance().IncrementCounter(
                            "detectbase_errors_total", { { "type", "setting_callback" } } );
                    }
                }
            }
        }

        // Internal update logic — 데이터 업데이트만 처리. callback 호출은 Update() 가 락 해제 후 책임.
        // 호출자가 data_update_mtx_ 를 hold 했다고 가정 (need_data_update_lock_ 인 경우).
        bool UpdateInternal( const nlohmann::json& update_json )
        {
            T prev_setting;
            bool was_initialized = this->is_initialized_;
            if( was_initialized ){
                prev_setting = this->setting_data_; // restore on failure 용 backup
            }

            if( this->setting_data_.UpdateFromJson( update_json ) == true ){
                this->is_initialized_ = true;
                return true;
            }
            else {
                if( was_initialized ){
                    this->setting_data_ = prev_setting;
                    MLOG_WARN("Update failed, restoring previous setting data.");
                } else {
                    MLOG_WARN("Initial update failed.");
                }
                return false;
            }
        }

    private:
        bool is_initialized_; // Has UpdateFromJson succeeded at least once?

        // Data Protection
        bool need_data_update_lock_;
        mutable std::mutex data_update_mtx_; // Protects setting_data_ if needed
        T setting_data_;                     // The actual setting data

        // Callback Management
        mutable std::mutex callback_mutex_; // Protects callbacks_ map
        std::unordered_map<std::string, SettingUpdateCallback> callbacks_;
    };
}