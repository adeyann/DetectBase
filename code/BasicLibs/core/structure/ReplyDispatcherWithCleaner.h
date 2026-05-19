#pragma once

#include "ReplyDispatcher.h"
#include "SafeThread.h"

#include <chrono>
#include <thread>
#include <atomic>
#include <memory>
#include <condition_variable>
#include <mutex>

namespace MGEN
{
    // V-05: chrono_literals 헤더에서 노출 안 함. default 인자는 std::chrono::milliseconds 명시.

    /**
     * @brief 내부 SafeThread를 활용해 주기적으로 remove_expired를 수행하는 확장형 Dispatcher
     * @tparam T 응답 데이터 타입
     * @tparam U UUID 타입 (기본: std::string)
     */
    template <typename T, typename U = std::string>
    class ReplyDispatcherWithCleaner
    {
    public:
        using BaseDispatcher = ReplyDispatcher<T, U>;

        ReplyDispatcherWithCleaner() = default;

        ~ReplyDispatcherWithCleaner()
        {
            StopAutoCleanup(); // 소멸자에서 안전 종료
        }

        // Dispatcher 기능 위임
        void set_reply(const U& uuid, const T& data) {
            dispatcher_.set_reply(uuid, data);
        }

        std::optional<T> wait_and_get( const U& uuid, unsigned int timeout_ms ) {
            auto wait_duration = std::chrono::milliseconds( timeout_ms );
            return dispatcher_.wait_and_get(uuid, wait_duration);
        }

        std::optional<T> wait_and_get( const U& uuid, std::chrono::milliseconds wait_duration ) {
            return dispatcher_.wait_and_get(uuid, wait_duration);
        }

        void terminate() {
            dispatcher_.terminate();
        }

        bool is_exist_in_entry(const U& uuid) const noexcept {
            return dispatcher_.is_exist_in_entry(uuid);
        }

        size_t total_size() const noexcept {
            return dispatcher_.total_size();
        }

        void remove_expired( unsigned int expiration_ms ) {
            dispatcher_.remove_expired(expiration_ms);
        }

        void CleanupRunner( void )
        {
            auto&  running = cleaner_thread_.GetRunningFlag();
            while( running.load() == true )
            {
                dispatcher_.remove_expired(expiration_time_ms_);

                // wait
                std::unique_lock<std::mutex> lck { this->mtx_ };
                this->cond_.wait_for( lck, interval_time_ms_, [&] {
                    return running.load() == false; }
                );

                // ⬇ 반드시 wait 이후에도 재검사 필요 (running=false && notify_missed 방지)
                if( running.load() == false ) break;
            }
        }

        void CleanupCloser( void )
        {
            std::unique_lock<std::mutex> lck { this->mtx_ };
            this->cond_.notify_all();
        }

        /**
         * @brief 주기적으로 remove_expired를 호출하는 자동 청소 루틴 시작
         * @param expiration_ms 만료 기준 시간 (밀리초)
         * @param interval_ms 주기 (예: 30초 = 30000)
         */
        void StartAutoCleanup( std::chrono::milliseconds expiration_ms = std::chrono::milliseconds( 30'000 ),
                               std::chrono::milliseconds interval_ms   = std::chrono::milliseconds( 30'000 ) )
        {
            if (interval_ms < std::chrono::milliseconds( 100 )) interval_ms = std::chrono::milliseconds( 100 ); // 과도한 루프 방지

            this->expiration_time_ms_ = expiration_ms;
            this->interval_time_ms_   = interval_ms;

            cleaner_thread_.SetThreadFunctions(
                // runner
                std::bind( &ReplyDispatcherWithCleaner::CleanupRunner, this ),
                // closer
                std::bind( &ReplyDispatcherWithCleaner::CleanupCloser, this )
            );

            cleaner_thread_.Start();
        }

        void StartAutoCleanup( unsigned int expiration_ms = 30'000, unsigned int interval_ms = 30'000 )
        {
            auto exp_dur = std::chrono::milliseconds( expiration_ms );
            auto itv_dur = std::chrono::milliseconds( interval_ms   );

            this->StartAutoCleanup( exp_dur, itv_dur );
        }

        /**
         * @brief 내부 청소 쓰레드를 정지시킴 (즉시 정지 및 join)
         */
        void StopAutoCleanup()
        {
            cleaner_thread_.Stop();
        }

    private:
        BaseDispatcher            dispatcher_;     ///< 실질적인 응답 디스패처
        SafeThread                cleaner_thread_; ///< remove_expired()를 수행하는 백그라운드 스레드
        std::chrono::milliseconds expiration_time_ms_ = std::chrono::milliseconds( 30'000 ); ///< 응답 유지 최대 시간
        std::chrono::milliseconds interval_time_ms_   = std::chrono::milliseconds( 30'000 ); ///< 정리 주기
        std::condition_variable   cond_;
        mutable std::mutex        mtx_;
    };
}
