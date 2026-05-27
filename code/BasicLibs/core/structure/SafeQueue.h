#ifndef _MGEN_SAFE_QUEUE_H_
#define _MGEN_SAFE_QUEUE_H_

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>

#include <iostream>
#include <chrono>


namespace MGEN
{
    // V-05: chrono_literals 헤더에서 노출 안 함 (CLAUDE.md "using namespace std (in headers) 금지").
    // 헤더 사용처는 std::chrono::milliseconds 직접 사용. literal 필요한 .cpp 에서 별도 using.

    /**
     * Thread-safe reader-wirter data queuing structure in a multi-thread
     */
    template <typename T>
    class SafeQueue
    {
    public:
        // Can only be used as default constructor
        explicit SafeQueue( void ) noexcept
            : q() // queue
            , m() // mutex
            , c() // condition_variables
        {}

        // Copy constructor prohibited
        SafeQueue( const SafeQueue& ) = delete;

        // Move constructor prohibited
        SafeQueue( SafeQueue&& ) = delete;

        // Copy assignments prohibited
        SafeQueue& operator=( const SafeQueue& ) = delete;

        // Move assignments prohibited
        SafeQueue& operator=( SafeQueue&& ) = delete;

        // Default destructor
        ~SafeQueue() = default;

        // 큐 capacity 한계 설정. 0 = 무제한 (default). 한계 초과 시 enqueue 가 oldest 를 drop.
        void SetMaxSize( size_t n ) noexcept
        {
            std::lock_guard<std::mutex> lck { m };
            max_size_ = n;
        }

        // 데이터를 복사하여 저장 → 원본이 변경되더라도 안전
        // MO-1 (2026-05-27): notify_one() 을 lock 밖으로 이동 (표준 권장 패턴).
        //   wakeup 받은 dequeue 가 lock 재획득 시 contention 미세 감소.
        void enqueue_copy( const T& t ) noexcept
        {
            {
                std::lock_guard<std::mutex> lck { m };
                if( max_size_ > 0 && q.size() >= max_size_ ) {
                    q.pop_front(); // drop oldest — 메모리 무한 증가 차단
                    drop_count_.fetch_add( 1, std::memory_order_relaxed );
                }
                q.push_back( t ); // copy
            }
            c.notify_one();
        }

        // 데이터를 이동하여 저장 → 소유권을 가져감
        void enqueue_move( T&& t ) noexcept
        {
            {
                std::lock_guard<std::mutex> lck { m };
                if( max_size_ > 0 && q.size() >= max_size_ ) {
                    q.pop_front(); // drop oldest
                    drop_count_.fetch_add( 1, std::memory_order_relaxed );
                }
                q.push_back( std::move(t) ); // move
            }
            c.notify_one();
        }

        // drop_oldest 누적 카운터 (cap 도달 시 oldest item 제거).
        uint64_t GetDropCount() const noexcept { return drop_count_.load( std::memory_order_relaxed ); }

        // dequeue() throw-기반 함수는 제거됨 (CLAUDE.md "C++ exceptions 사용 금지" 준수).
        // 사용처는 모두 dequeue_wait_for(timeout) 으로 마이그레이션 — 종료/타임아웃 시 std::nullopt 반환.

        // 종료/타임아웃 시 std::nullopt 반환. 정상 데이터는 std::optional<T>(value).
        std::optional<T> dequeue_wait_for( std::chrono::milliseconds wait_duration )
        {
            std::unique_lock<std::mutex> lck { m };

            if( b_terminate ) {
                return std::nullopt;
            }

            if( !c.wait_for( lck, wait_duration, [this] { return ( q.empty() == false ) || ( b_terminate == true ); } ) ) {
                return std::nullopt;
            }

            if( b_terminate ) {
                return std::nullopt;
            }

            T val = std::move( q.front() );
            q.pop_front();

            return val;
        }

        size_t size() const noexcept
        {
            std::lock_guard<std::mutex> lck { m };
            return q.size();
        }

        bool empty() const noexcept
        {
            return this->size() == 0;
        }

        void terminate() noexcept
        {
            std::lock_guard<std::mutex> lck { m };
            b_terminate = true;
            c.notify_all();
        }

        bool is_terminated() const noexcept
        {
            std::lock_guard<std::mutex> lck { m };
            return b_terminate;
        }

        /**
         * @brief Clears the queue, applying a specific action to each element.
         *
         * This function removes all elements currently in the queue. For each element,
         * the provided 'action' callable is invoked with the element (moved).
         * This is useful for cleaning up resources held by elements (e.g., freeing pointers)
         * especially after the queue has been terminated or is no longer needed.
         *
         * This operation minimizes lock holding time by moving the queue contents
         * to a temporary deque before applying the action.
         *
         * @tparam Action A callable type (e.g., lambda, function pointer) that accepts T&&.
         * It should handle the cleanup logic for an element.
         * @param action The action to perform on each element before it's discarded.
         * Exceptions thrown by the action are caught and printed to stderr,
         * but processing continues for subsequent elements.
         */
        template <typename Action>
        void clear_with_action(Action action)
        {
            std::deque<T> temp_q;

            { // Critical section to quickly swap out the internal queue
                std::lock_guard<std::mutex> lck{m};
                // Swap the internal queue with the temporary one.
                // The internal queue 'q' becomes empty almost instantly.
                q.swap(temp_q);
                // Mutex is released here as lock_guard goes out of scope.
            }

            // Now process the elements from the temporary queue without holding the lock.
            while (!temp_q.empty()) {
                T element = std::move(temp_q.front());
                temp_q.pop_front();
                try {
                    // Apply the user-provided cleanup action
                    action(std::move(element));
                } catch (const std::exception& e) {
                    // Catch exceptions from the action to prevent interruption
                    // of the cleanup process for other elements. Log the error.
                    std::cerr << "SafeQueue::clear_with_action: Exception caught during cleanup action: " << e.what() << std::endl;
                } catch (...) {
                    // Catch any other types of exceptions
                    std::cerr << "SafeQueue::clear_with_action: Unknown exception caught during cleanup action." << std::endl;
                }
            }
            // temp_q is now empty and its destructor will run.
        }

        /**
         * @brief Clears all elements in the queue without applying any actions.
         *
         * This function efficiently removes all elements from the queue by swapping
         * the internal deque with an empty one. It is thread-safe and ensures minimal
         * lock holding time.
         */
        void clear_without_action()
        {
            std::deque<T> temp_q;
            {
                std::lock_guard<std::mutex> lck{m};
                q.swap(temp_q); // 내부 큐와 비어있는 큐를 스왑하여 빠르게 비움
            }
            // temp_q는 여기서 파괴되며 메모리 정리됨
        }

    private:
        std::deque<T>           q;
        mutable std::mutex      m;
        std::condition_variable c;
        bool                    b_terminate = false;
        size_t                  max_size_   = 0; // 0 = 무제한
        std::atomic<uint64_t>   drop_count_ { 0 };
    };

    template <class T>
    using sptrSafeQueue = std::shared_ptr<SafeQueue<T>>;

    template <class T>
    using uptrSafeQueue = std::unique_ptr<SafeQueue<T>>;

}


#endif
