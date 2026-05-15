#ifndef __MGEN_METRICS_REGISTRY_H__
#define __MGEN_METRICS_REGISTRY_H__

/** -------------------------------------------------------
 *  P54 — Prometheus metrics registry
 * --------------------------------------------------------
 *  HTTP /metrics endpoint 노출 (port 기본 9090).
 *  Prometheus 서버가 pull 방식으로 수집.
 *
 *  사용 흐름:
 *      // 1. 시스템 시작 시 (Service init)
 *      MGEN::MetricsRegistry::Instance().Initialize( 9090 );
 *
 *      // 2. 메트릭 등록 (한 번만)
 *      MGEN::MetricsRegistry::Instance().RegisterGauge(
 *          "detectbase_dfps", "Detection FPS per camera", { "cam" } );
 *
 *      // 3. 측정 시점에 update
 *      MGEN::MetricsRegistry::Instance().SetGauge(
 *          "detectbase_dfps", { { "cam", "658" } }, 13.3 );
 *
 *      // 4. 종료 시
 *      MGEN::MetricsRegistry::Instance().Shutdown();
 *
 *  pImpl 디자인: 본 헤더는 prometheus 헤더에 의존하지 않음.
 *  -> BasicLibs 의 다른 사용자에게 prometheus 의존성 노출 안 됨.
 * -------------------------------------------------------- */

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace MGEN
{
    class MetricsRegistry
    {
    public:
        // 싱글턴 인스턴스 접근. 처음 호출 시 생성, 이후 동일 인스턴스 반환.
        static MetricsRegistry& Instance();

        // HTTP exposer 시작. 이미 초기화됐다면 무시 (idempotent).
        // port 0 또는 음수면 비활성 상태로 둠 (메트릭 측정 자체는 동작).
        void Initialize( int port );

        // HTTP exposer 정리. 메트릭 측정은 계속 가능하지만 endpoint 응답 안 함.
        void Shutdown();

        // -- 메트릭 등록 (한 번만 호출 권장. 중복 호출은 무시) ---------------
        // F-F6-06: label_keys 는 의도 표현용 (prometheus-cpp 의 BuildX 에 전달 안 됨, label 은 Add(labels) 시 결정).
        // default {} 로 호출 시 라벨 없는 메트릭. 기존 호출자 호환성 유지.
        void RegisterCounter   ( const std::string& name, const std::string& help,
                                 const std::vector<std::string>& label_keys = {} );
        void RegisterGauge     ( const std::string& name, const std::string& help,
                                 const std::vector<std::string>& label_keys = {} );
        void RegisterHistogram ( const std::string& name, const std::string& help,
                                 const std::vector<std::string>& label_keys,
                                 const std::vector<double>&      buckets );

        // -- 측정 (다중 thread 동시 호출 안전. prometheus-cpp 자체 atomic) --
        void IncrementCounter ( const std::string& name,
                                const std::map<std::string, std::string>& labels,
                                double value = 1.0 );
        void SetGauge         ( const std::string& name,
                                const std::map<std::string, std::string>& labels,
                                double value );
        void IncrementGauge   ( const std::string& name,
                                const std::map<std::string, std::string>& labels,
                                double value = 1.0 );
        void DecrementGauge   ( const std::string& name,
                                const std::map<std::string, std::string>& labels,
                                double value = 1.0 );
        void ObserveHistogram ( const std::string& name,
                                const std::map<std::string, std::string>& labels,
                                double value );

        // copy/move 금지 (싱글턴)
        MetricsRegistry( const MetricsRegistry& )            = delete;
        MetricsRegistry& operator=( const MetricsRegistry& ) = delete;
        MetricsRegistry( MetricsRegistry&& )                 = delete;
        MetricsRegistry& operator=( MetricsRegistry&& )      = delete;

    private:
        MetricsRegistry();
        ~MetricsRegistry();

        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
} // namespace MGEN

#endif // __MGEN_METRICS_REGISTRY_H__
