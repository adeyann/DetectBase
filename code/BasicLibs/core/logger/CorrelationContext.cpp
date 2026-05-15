#include "CorrelationContext.h"

#include <atomic>
#include <chrono>
#include <cstdio>

namespace MGEN
{
    namespace
    {
        // thread-local 저장소. 빈 문자열 = "미설정".
        thread_local std::string current_correlation_id;

        // 전역 단조 증가 시퀀스 (서로 다른 thread / 서로 다른 ms 의 충돌 방지).
        std::atomic<uint64_t> sequence_counter { 0 };
    }

    namespace CorrelationContext
    {
        const std::string& Get() noexcept
        {
            return current_correlation_id;
        }

        void Set( std::string id ) noexcept
        {
            current_correlation_id = std::move( id );
        }

        std::string NewId( std::string_view prefix )
        {
            const auto unix_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch() ).count();
            const auto seq = sequence_counter.fetch_add( 1, std::memory_order_relaxed );

            char buf[64];
            std::snprintf( buf, sizeof( buf ), "%.*s-%lld-%llu",
                static_cast<int>( prefix.size() ), prefix.data(),
                static_cast<long long>( unix_ms ),
                static_cast<unsigned long long>( seq ) );
            return std::string { buf };
        }
    }

    CorrelationScope::CorrelationScope( std::string_view prefix )
        : previous_( current_correlation_id )
    {
        current_correlation_id = CorrelationContext::NewId( prefix );
    }

    CorrelationScope::~CorrelationScope()
    {
        current_correlation_id = std::move( previous_ );
    }
} // namespace MGEN
