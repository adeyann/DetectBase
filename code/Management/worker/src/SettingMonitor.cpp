#include "SettingMonitor.h"
#include "MgenLogger.h"
#include <vector>

namespace MGEN
{
    void SettingMonitor::ClearAllSubscriptions() {
        // 해제 함수들을 복사하여 락 외부에서 호출 (재진입 또는 데드락 방지)
        std::vector<std::function<bool()>> unregister_funcs_to_call;
        {
            std::lock_guard lock(subscription_mutex_); // 맵 접근 보호
            if (!active_subscriptions_.empty()) {
                unregister_funcs_to_call.reserve(active_subscriptions_.size());
                // map 에서 함수 객체를 이동(move)하여 벡터에 저장
                for (auto& pair : active_subscriptions_) {
                    if(pair.second) {
                        unregister_funcs_to_call.push_back(std::move(pair.second));
                    }
                }
                active_subscriptions_.clear();
            }
        } // 뮤텍스 락 해제

        // 락 외부에서 실제 해제 함수들 호출
        for (auto& func : unregister_funcs_to_call) {
            if (func) {
                func();
            }
        }
    }

    SettingMonitor::~SettingMonitor() {
        // 멤버 destroy 전에 callback registry 정리 (idempotent — 이미 명시 호출됐어도 안전)
        ClearAllSubscriptions();
    }

    bool SettingMonitor::Unsubscribe(const std::string& uuid_to_remove) {
        if (uuid_to_remove.empty()) {
            return false;
        }

        std::function<bool()> func_to_call;
        bool found = false;

        // 맵에서 해당 UUID를 찾아 함수 객체를 꺼내고 맵에서 제거 (락 범위 최소화)
        {
            std::lock_guard lock(subscription_mutex_);
            auto it = active_subscriptions_.find(uuid_to_remove);
            if (it != active_subscriptions_.end()) {
                found = true;
                func_to_call = std::move(it->second);
                active_subscriptions_.erase(it);
            }
        } // 뮤텍스 락 해제

        if (found) {
            if (func_to_call) {
                return func_to_call();
            }
            // map 에 null 함수가 저장된 비정상 상황
            MLOG_ERROR("SettingMonitor: Found entry for UUID %s but unregister function is invalid!", uuid_to_remove.c_str());
            return false;
        }
        return false;
    }

} // namespace MGEN
