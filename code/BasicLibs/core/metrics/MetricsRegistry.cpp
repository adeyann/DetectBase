#include "MetricsRegistry.h"
#include "MgenLogger.h"

#include <prometheus/counter.h>
#include <prometheus/exposer.h>
#include <prometheus/family.h>
#include <prometheus/gauge.h>
#include <prometheus/histogram.h>
#include <prometheus/registry.h>

#include <mutex>
#include <unordered_map>

namespace MGEN
{
    struct MetricsRegistry::Impl
    {
        std::shared_ptr<prometheus::Registry>   registry;
        std::unique_ptr<prometheus::Exposer>    exposer;

        // 메트릭 family 보관. name -> Family*.
        // Family<T> 는 Registry 가 소유하므로 pointer 만 들고 있으면 안전.
        std::unordered_map<std::string, prometheus::Family<prometheus::Counter>*>   counter_families;
        std::unordered_map<std::string, prometheus::Family<prometheus::Gauge>*>     gauge_families;
        std::unordered_map<std::string, prometheus::Family<prometheus::Histogram>*> histogram_families;

        // Histogram 의 bucket 정의는 family 등록 시점에 한 번 주어지지만,
        // Add(labels) 호출 시 사용 안 함. 대신 family 에 default 가 박혀있어야 함.
        // 따라서 family 별로 buckets 보관해서 ObserveHistogram 시점에 사용.
        std::unordered_map<std::string, std::vector<double>> histogram_buckets;

        // 등록 / Add 동시 안전성 보호용
        std::mutex mtx;

        bool initialized = false;
        int  port        = 0;
    };

    MetricsRegistry& MetricsRegistry::Instance()
    {
        static MetricsRegistry instance;
        return instance;
    }

    MetricsRegistry::MetricsRegistry()
        : impl_( std::make_unique<Impl>() )
    {
        impl_->registry = std::make_shared<prometheus::Registry>();
    }

    MetricsRegistry::~MetricsRegistry()
    {
        Shutdown();
    }

    void MetricsRegistry::Initialize( int port )
    {
        std::lock_guard<std::mutex> lck { impl_->mtx };

        if( impl_->initialized ) {
            MLOG_WARN( "MetricsRegistry::Initialize already called (port=%d). Ignoring.", impl_->port );
            return;
        }

        impl_->port = port;

        if( port <= 0 ) {
            MLOG_INFO( "MetricsRegistry: Initialize with port=%d (HTTP exposer disabled).", port );
            impl_->initialized = true;
            return;
        }

        // HTTP exposer 시작 (civetweb backend).
        // bind: 0.0.0.0:<port> — docker container 안에서 외부 접근 허용.
        try {
            const std::string bind_addr = "0.0.0.0:" + std::to_string( port );
            impl_->exposer = std::make_unique<prometheus::Exposer>( bind_addr );
            impl_->exposer->RegisterCollectable( impl_->registry );
            impl_->initialized = true;
            MLOG_INFO( "MetricsRegistry: HTTP exposer started on %s/metrics", bind_addr.c_str() );
        }
        catch( const std::exception& e ) {
            MLOG_ERROR( "MetricsRegistry: Failed to start exposer on port %d: %s", port, e.what() );
            impl_->exposer.reset();
            // 측정 자체는 계속 가능. exposer 만 비활성.
            impl_->initialized = true;
        }
    }

    void MetricsRegistry::Shutdown()
    {
        std::lock_guard<std::mutex> lck { impl_->mtx };

        if( impl_->exposer ) {
            impl_->exposer.reset();
            MLOG_INFO( "MetricsRegistry: HTTP exposer stopped." );
        }
        impl_->initialized = false;
    }

    // ---------------- 등록 ----------------------------------------------

    void MetricsRegistry::RegisterCounter(
        const std::string& name, const std::string& help,
        const std::vector<std::string>& /* label_keys */ )
    {
        std::lock_guard<std::mutex> lck { impl_->mtx };

        if( impl_->counter_families.count( name ) > 0 )
            return; // 이미 등록됨

        auto& family = prometheus::BuildCounter()
            .Name( name )
            .Help( help )
            .Register( *impl_->registry );

        impl_->counter_families[name] = &family;
    }

    void MetricsRegistry::RegisterGauge(
        const std::string& name, const std::string& help,
        const std::vector<std::string>& /* label_keys */ )
    {
        std::lock_guard<std::mutex> lck { impl_->mtx };

        if( impl_->gauge_families.count( name ) > 0 )
            return;

        auto& family = prometheus::BuildGauge()
            .Name( name )
            .Help( help )
            .Register( *impl_->registry );

        impl_->gauge_families[name] = &family;
    }

    void MetricsRegistry::RegisterHistogram(
        const std::string& name, const std::string& help,
        const std::vector<std::string>& /* label_keys */,
        const std::vector<double>& buckets )
    {
        std::lock_guard<std::mutex> lck { impl_->mtx };

        if( impl_->histogram_families.count( name ) > 0 )
            return;

        auto& family = prometheus::BuildHistogram()
            .Name( name )
            .Help( help )
            .Register( *impl_->registry );

        impl_->histogram_families[name] = &family;
        impl_->histogram_buckets[name]  = buckets;
    }

    // ---------------- 측정 ----------------------------------------------

    void MetricsRegistry::IncrementCounter(
        const std::string& name,
        const std::map<std::string, std::string>& labels,
        double value )
    {
        std::lock_guard<std::mutex> lck { impl_->mtx };

        auto it = impl_->counter_families.find( name );
        if( it == impl_->counter_families.end() )
            return;

        it->second->Add( labels ).Increment( value );
    }

    void MetricsRegistry::SetGauge(
        const std::string& name,
        const std::map<std::string, std::string>& labels,
        double value )
    {
        std::lock_guard<std::mutex> lck { impl_->mtx };

        auto it = impl_->gauge_families.find( name );
        if( it == impl_->gauge_families.end() )
            return;

        it->second->Add( labels ).Set( value );
    }

    void MetricsRegistry::IncrementGauge(
        const std::string& name,
        const std::map<std::string, std::string>& labels,
        double value )
    {
        std::lock_guard<std::mutex> lck { impl_->mtx };

        auto it = impl_->gauge_families.find( name );
        if( it == impl_->gauge_families.end() )
            return;

        it->second->Add( labels ).Increment( value );
    }

    void MetricsRegistry::DecrementGauge(
        const std::string& name,
        const std::map<std::string, std::string>& labels,
        double value )
    {
        std::lock_guard<std::mutex> lck { impl_->mtx };

        auto it = impl_->gauge_families.find( name );
        if( it == impl_->gauge_families.end() )
            return;

        it->second->Add( labels ).Decrement( value );
    }

    void MetricsRegistry::ObserveHistogram(
        const std::string& name,
        const std::map<std::string, std::string>& labels,
        double value )
    {
        std::lock_guard<std::mutex> lck { impl_->mtx };

        auto it = impl_->histogram_families.find( name );
        if( it == impl_->histogram_families.end() )
            return;

        const auto& buckets = impl_->histogram_buckets[name];
        it->second->Add( labels, buckets ).Observe( value );
    }
} // namespace MGEN
