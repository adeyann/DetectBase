#pragma once

#include <unordered_map> // std::unordered_map 사용
#include <mutex>         // std::mutex, std::lock_guard, std::unique_lock 사용
#include <condition_variable> // std::condition_variable 사용
#include <optional>      // std::optional 사용
#include <string>        // std::string 사용
#include <chrono>        // std::chrono 관련 기능 사용
#include <vector>        // std::vector 사용
#include <memory>        // std::unique_ptr, std::shared_ptr 사용
#include <cassert>       // assert 사용
#include <algorithm>     // std::move 등 사용

namespace MGEN
{
    // V-05: chrono_literals 헤더에서 노출 안 함. literal 사용은 .cpp 안 별도 using.

    /**
     * @brief UUID 기반 비동기 응답 매칭 Dispatcher (Sharded Lock version)
     * @details 각 UUID를 해시하여 독립적인 버킷에 할당함으로써 전역 락 경합을 방지합니다.
     * @tparam T 응답 데이터 타입
     * @tparam U UUID 타입 (기본 std::string, 현재 프로젝트에서는 UnitID 사용 가능)
     */
    template<typename T, typename U = std::string>
    class ReplyDispatcher
    {
    private:
        /**
         * @brief UUID별 응답 엔트리 구조체
         */
        struct ReplyEntry
        {
            mutable std::mutex              mutex;        ///< 데이터 접근 및 대기 보호용 뮤텍스
            std::condition_variable         cv;           ///< 응답 대기용 조건 변수
            std::optional<T>                data;         ///< 실제 응답 데이터
            std::chrono::steady_clock::time_point created_time = std::chrono::steady_clock::now(); ///< 생성 시각
        };

        /**
         * @brief 독립적인 락을 가지는 데이터 영역 단위
         */
        struct Bucket
        {
            mutable std::mutex                                 mutex; ///< 버킷 내 테이블 접근 보호용 뮤텍스
            std::unordered_map<U, std::shared_ptr<ReplyEntry>> table; ///< UUID -> ReplyEntry 맵
        };

    public:
        /**
         * @brief ReplyDispatcher 생성자
         * @param shard_count 샤딩할 버킷의 개수 (기본 16)
         */
        explicit ReplyDispatcher( size_t shard_count = 16 )
        {
            assert( shard_count > 0 ); // 샤드 개수는 0보다 커야 함
            _shard_count = shard_count;

            for( size_t i = 0; i < _shard_count; ++i ){
                _buckets.emplace_back( std::make_unique<Bucket>() );
            }
        }

        // 복사 및 이동 금지 (RAII 보존 및 뮤텍스 안전성)
        ReplyDispatcher( const ReplyDispatcher& )            = delete;
        ReplyDispatcher& operator=( const ReplyDispatcher& ) = delete;
        ReplyDispatcher( ReplyDispatcher&& )                 = delete;
        ReplyDispatcher& operator=( ReplyDispatcher&& )      = delete;

        ~ReplyDispatcher()
        {
            this->terminate();
        }

        /**
         * @brief 특정 UUID에 대한 응답 데이터를 설정하고 대기 중인 스레드에 알림.
         * @param uuid 응답을 식별하는 UUID.
         * @param reply_data 설정할 응답 데이터.
         */
        void set_reply( const U& uuid, const T& reply_data )
        {
            auto& bucket = _get_bucket( uuid );
            std::shared_ptr<ReplyEntry> entry;

            {
                std::lock_guard<std::mutex> lock{ bucket.mutex };

                auto [ it, inserted ] = bucket.table.try_emplace( uuid, nullptr );
                if( inserted ){
                    it->second = std::make_shared<ReplyEntry>();
                }
                entry = it->second;
            }

            // 방어적 코딩: entry가 nullptr일 수 없으나, 예외 상황 대비
            assert( entry != nullptr );

            {
                std::lock_guard<std::mutex> lock{ entry->mutex };
                entry->data = reply_data;
            }

            entry->cv.notify_all();
        }

        /**
         * @brief 특정 UUID에 대한 응답을 지정된 시간 동안 대기하고 결과를 반환.
         * @param uuid 대기할 응답의 UUID.
         * @param wait_duration 대기할 최대 시간.
         * @return 응답 데이터 또는 타임아웃 시 std::nullopt.
         */
        std::optional<T> wait_and_get( const U& uuid, std::chrono::milliseconds wait_duration )
        {
            auto& bucket = _get_bucket( uuid );
            std::shared_ptr<ReplyEntry> entry;

            {
                std::lock_guard<std::mutex> lock{ bucket.mutex };

                auto [ it, inserted ] = bucket.table.try_emplace( uuid, nullptr );
                if( inserted ){
                    it->second = std::make_shared<ReplyEntry>();
                }
                entry = it->second;
            }

            std::optional<T> result = std::nullopt;
            {
                std::unique_lock<std::mutex> lock{ entry->mutex };

                // Predicate를 사용하여 Spurious Wakeup 방지
                bool success = entry->cv.wait_for( lock, wait_duration, [ & ] {
                    return entry->data.has_value();
                } );

                if( success ){
                    result = std::move( entry->data );
                }
            }

            // 결과를 성공적으로 가져왔거나, 타임아웃되어도 1회성 요청이면 제거
            // 현재 프로젝트 로직상 wait_and_get 성공 시 데이터 정리를 수행함
            if( result.has_value() ){
                std::lock_guard<std::mutex> lock{ bucket.mutex };
                bucket.table.erase( uuid );
            }

            return result;
        }

        /**
         * @brief 특정 UUID에 대한 응답을 지정된 시간 동안 대기하고 결과를 반환.
         * @param uuid 대기할 응답의 UUID.
         * @param wait_duration_ms 대기할 최대 시간 (밀리초).
         * @return 응답 데이터 또는 타임아웃 시 std::nullopt.
         */
        std::optional<T> wait_and_get( const U& uuid, int wait_duration_ms )
        {
            return wait_and_get( uuid, std::chrono::milliseconds( wait_duration_ms ) );
        }

        /**
         * @brief Dispatcher를 종료하고 모든 대기 중인 스레드에 알림.
         */
        void terminate()
        {
            for( auto& bucket_ptr : _buckets ){
                std::vector<std::shared_ptr<ReplyEntry>> entries_to_notify;

                {
                    std::lock_guard<std::mutex> lock{ bucket_ptr->mutex };
                    for( auto& [ uuid, entry ] : bucket_ptr->table ){
                        entries_to_notify.push_back( entry );
                    }
                    bucket_ptr->table.clear();
                }

                for( auto& entry : entries_to_notify ){
                    if( entry ){
                        {
                            std::lock_guard<std::mutex> lock{ entry->mutex };
                            entry->data = std::nullopt; // 종료 신호
                        }
                        entry->cv.notify_all();
                    }
                }
            }
        }

        /**
         * @brief 특정 UUID에 해당하는 엔트리 존재 여부 반환.
         */
        bool is_exist_in_entry( const U& uuid ) const noexcept
        {
            auto& bucket = _get_bucket( uuid );
            std::lock_guard<std::mutex> lock{ bucket.mutex };
            return bucket.table.find( uuid ) != bucket.table.end();
        }

        /**
         * @brief 일정 시간 이상 경과된 응답 엔트리 제거.
         * @param expiration 만료 기준 시간.
         */
        void remove_expired( std::chrono::milliseconds expiration )
        {
            const auto now = std::chrono::steady_clock::now();

            for( auto& bucket_ptr : _buckets ){
                std::lock_guard<std::mutex> lock{ bucket_ptr->mutex };

                auto it = bucket_ptr->table.begin();
                while( it != bucket_ptr->table.end() ){
                    // entry가 존재하고 생성 시간이 만료 기준보다 오래된 경우 삭제
                    if( it->second && ( now - it->second->created_time > expiration ) ){
                        it = bucket_ptr->table.erase( it );
                    } else {
                        ++it;
                    }
                }
            }
        }

        /**
         * @brief 모든 bucket 의 table size 합산 — leak hunt 용 (pending reply entry 누적 감시).
         */
        size_t total_size() const noexcept
        {
            size_t total = 0;
            for( const auto& bucket_ptr : _buckets ){
                std::lock_guard<std::mutex> lock{ bucket_ptr->mutex };
                total += bucket_ptr->table.size();
            }
            return total;
        }

        /**
         * @brief 특정 UUID에 해당하는 응답 엔트리를 수동 제거.
         */
        void remove( const U& uuid )
        {
            auto& bucket = _get_bucket( uuid );
            std::shared_ptr<ReplyEntry> entry_to_notify;

            {
                std::lock_guard<std::mutex> lock{ bucket.mutex };
                auto it = bucket.table.find( uuid );
                if( it != bucket.table.end() ){
                    entry_to_notify = it->second;
                    bucket.table.erase( it );
                }
            }

            if( entry_to_notify ){
                entry_to_notify->cv.notify_all();
            }
        }

    private:
        /**
         * @brief UUID를 기반으로 대상 버킷을 결정하는 내부 함수.
         * @param uuid 대상 UUID.
         * @return 대상 버킷 객체의 참조.
         */
        Bucket& _get_bucket( const U& uuid ) const
        {
            // std::hash를 사용하여 UUID에 대한 인덱스 산출
            size_t index = std::hash<U>{}( uuid ) % _shard_count;
            return *_buckets[ index ];
        }

    private:
        size_t                               _shard_count; ///< 샤드 버킷 개수
        std::vector<std::unique_ptr<Bucket>> _buckets;     ///< 샤딩된 버킷 리스트
    };
}