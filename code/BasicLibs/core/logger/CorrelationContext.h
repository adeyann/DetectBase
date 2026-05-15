#ifndef __MGEN_CORRELATION_CONTEXT_H__
#define __MGEN_CORRELATION_CONTEXT_H__

/** -------------------------------------------------------
 *  P53 — Correlation ID context (thread-local)
 * --------------------------------------------------------
 *  한 요청 / 이벤트가 여러 모듈을 거쳐갈 때 동일 ID 를 부여해서
 *  로그 lifecycle 추적을 가능하게 함.
 *
 *  사용 예 (진입점):
 *      void OnSocketIoMessage(const json& msg) {
 *          MGEN::CorrelationScope scope { "evt" };  // 새 ID 자동 발급
 *          // 이 thread 의 모든 MLOG 호출이 동일 correlation_id 보유
 *          ProcessMessage(msg);
 *      }
 *
 *  scope 가 끝나면 이전 ID 로 복원 — thread-local 누설 차단.
 * -------------------------------------------------------- */

#include <string>
#include <string_view>

namespace MGEN
{
    // thread-local 현재 correlation_id 접근.
    // 빈 문자열 = "ID 미설정" (로그에 필드 안 찍음).
    namespace CorrelationContext
    {
        // 현재 thread 의 correlation_id 조회.
        const std::string& Get() noexcept;

        // 현재 thread 의 correlation_id 직접 설정 (보통 CorrelationScope 가 호출).
        void Set( std::string id ) noexcept;

        // 새 ID 생성. format: "<prefix>-<unix_ms>-<seq>"
        // prefix 예: "evt" (SocketIO event), "req" (REST request), "sys" (system).
        std::string NewId( std::string_view prefix );
    } // namespace CorrelationContext

    // RAII scope: ctor 에서 새 ID set, dtor 에서 이전 ID 로 restore.
    // 동일 thread 안에서 nested scope 도 안전 (이전 값 보존).
    //
    // 명시적 ID 가 필요하면 CorrelationContext::Set() 직접 사용.
    class CorrelationScope
    {
    public:
        // 새 ID 자동 발급. prefix 예: "evt", "req", "sys".
        explicit CorrelationScope( std::string_view prefix );

        ~CorrelationScope();

        // copy/move 금지 — RAII scope 는 한 곳에서만 소유
        CorrelationScope( const CorrelationScope& )            = delete;
        CorrelationScope& operator=( const CorrelationScope& ) = delete;
        CorrelationScope( CorrelationScope&& )                 = delete;
        CorrelationScope& operator=( CorrelationScope&& )      = delete;

    private:
        std::string previous_;
    };
} // namespace MGEN

#endif // __MGEN_CORRELATION_CONTEXT_H__
