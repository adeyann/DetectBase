#pragma once // Include guard

#include <functional>
#include <string>
#include <memory>
#include <map>      // 여러 구독 정보 저장을 위해 map 사용
#include <mutex>    // 구독 정보 접근 동기화를 위해 mutex 사용
#include <type_traits> // For std::is_same_v
#include <utility>     // For std::forward

#include "MgenTypes.h" // For UnitID
#include "MgenLogger.h" // For MLOG_...

// SettingManager 클래스의 전체 정의 포함 필요
// Register...Callback 및 Unregister...Callback 함수들을 호출해야 하기 때문
#include "SettingManager.h"

namespace MGEN
{
    class SettingMonitor
    {
    private:
        // 여러 구독 정보를 저장: UUID -> 해당 구독 해제 함수
        std::map<std::string, std::function<bool()>> active_subscriptions_;
        mutable std::mutex subscription_mutex_; // active_subscriptions_ 접근 보호용 뮤텍스

        // 복사 및 이동 방지
        SettingMonitor(const SettingMonitor&) = delete;
        SettingMonitor& operator=(const SettingMonitor&) = delete;
        SettingMonitor(SettingMonitor&&) = delete;
        SettingMonitor& operator=(SettingMonitor&&) = delete;

    protected:
        // 베이스 클래스 생성자
        SettingMonitor() = default;

    public:
        /**
         * @brief 특정 설정 데이터 타입에 대한 콜백을 등록합니다.
         * @tparam T_SettingData 구독할 설정 데이터의 타입 (예: MGEN::ServerSettingData).
         * @tparam CallbackFunc 콜백 함수의 타입 (보통 람다). const T_SettingData& 를 인자로 받아야 함.
         * @param sm SettingManager 인스턴스 shared_ptr.
         * @param callback 설정 변경 시 호출될 콜백 함수.
         * @param unit_id Multi-unit 설정 타입의 경우 대상 Unit ID (ServerSetting 등에는 불필요).
         * @return 성공 시 콜백을 식별하는 UUID 문자열, 실패 시 빈 문자열.
         */
        template <typename T_SettingData, typename CallbackFunc, typename ManagerType = SettingManager>
        std::string SubscribeSetting(
            CallbackFunc&& callback,
            MGEN::Type::UnitID unit_id = UNIT_ID_NOT_SET,
            std::shared_ptr<ManagerType> sm = MGEN::GetSettingManager() ) // 기본값 사용
        {
            if (!sm) {
                MLOG_WARN("SettingMonitor: SubscribeSetting called with null SettingManager.");
                return ""; // 실패 시 빈 UUID 반환
            }

            // 전달받은 콜백 함수를 std::function 으로 래핑 (타입 일치 및 저장 용이)
            std::function<void(const T_SettingData&)> wrapped_callback = std::forward<CallbackFunc>(callback);

            std::string received_uuid;
            // 해제 함수 대리자 (실제 SettingManager의 Unregister 함수를 호출)
            std::function<bool(const std::string&)> unregister_delegate;

            // --- 타입 T_SettingData 에 따라 Register 함수 호출 및 해제 대리자 생성 ---
            // 정책: 동적 변경 가능 설정만 subscribe 허용. ServerSettingData 는 의도적으로 미지원
            //   — ServerSetting 은 program 시작 시 1회 로드되는 read-only 설정 (변경 시 재시작 필요).
            //   향후 ServerSetting 도 hot-reload 가 필요하면 분기 추가 + Register/UnregisterServerSettingCallback API 도입.
            if constexpr (std::is_same_v<T_SettingData, MGEN::ScheduleSettingData>) {
                received_uuid = sm->RegisterScheduleSettingCallback(unit_id, wrapped_callback);
                unregister_delegate = [sm_capture = sm, unit_id](const std::string& uuid) {
                    return sm_capture->UnregisterScheduleSettingCallback(unit_id, uuid);
                };
            } else if constexpr (std::is_same_v<T_SettingData, MGEN::ExcludeCamSettingData>) {
                received_uuid = sm->RegisterExcludeCamSettingCallback(unit_id, wrapped_callback);
                unregister_delegate = [sm_capture = sm, unit_id](const std::string& uuid) {
                    return sm_capture->UnregisterExcludeCamSettingCallback(unit_id, uuid);
                };
            }
            else {
                MLOG_ERROR("SettingMonitor: Unsupported SettingData type provided for subscription: %s", typeid(T_SettingData).name());
                return "";
            }
            // --- 타입 분기 끝 ---

            // SettingManager 등록 실패 시
            if (received_uuid.empty()) {
                // MLOG_ERROR("SettingMonitor: Failed to register callback with SettingManager (UnitID: %d, Type: %s).", unit_id, typeid(T_SettingData).name());
                return ""; // 실패 시 빈 UUID 반환
            }

            // 최종 해제 함수 생성 (UUID 와 해제 대리자 캡처)
            std::function<bool()> final_unregister_func =
                [unregister_delegate, uuid = received_uuid]() -> bool {
                    // 소멸 또는 명시적 호출 시 실행될 로그
                    // MLOG_INFO("SettingMonitor: Unregistering callback (UUID: %s)", uuid.c_str());
                    return unregister_delegate(uuid); // 캡처된 대리자 호출
                };

            // 생성된 정보를 맵에 저장 (뮤텍스 사용)
            {
                std::lock_guard lock(subscription_mutex_);
                active_subscriptions_[received_uuid] = std::move(final_unregister_func);
            }

            // MLOG_INFO("SettingMonitor: Successfully subscribed (UUID: %s, UnitID: %d, Type: %s).",
            //         received_uuid.c_str(), unit_id, typeid(T_SettingData).name());
            return received_uuid; // 성공 시 UUID 반환
        }

    public:
        // 가상 소멸자 선언 (구현은 .cpp 파일)
        virtual ~SettingMonitor();

        /**
         * @brief 특정 UUID를 가진 콜백 구독을 명시적으로 해제합니다.
         * @param uuid_to_remove 해제할 콜백의 UUID.
         * @return 해제 성공 시 true, 실패(UUID 없음 등) 시 false.
         */
        bool Unsubscribe(const std::string& uuid_to_remove); // 선언만 헤더에

        /**
         * @brief 등록된 모든 구독을 한 번에 해제. 멤버 destroy 전에 명시 호출 권장
         *        (raw [this] 캡처 callback 의 UAF 차단).
         */
        void ClearAllSubscriptions();
    };

} // namespace MGEN
