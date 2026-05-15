#pragma once

#include "SafeThread.h"
#include "SafeQueue.h"
#include "MgenTypes.h"

#include <memory>
#include <mutex>
#include <unordered_map>
#include <condition_variable>
#include <atomic>



namespace MGEN
{
    constexpr int DFPS_CHECK_INTERVAL_SECONDS = 10;

    class InferenceCounter
    {
    public:
        explicit InferenceCounter();
        ~InferenceCounter();

        void Start( void );
        void Stop( void );

        void Regist  ( const MGEN::Type::UnitID unit_id );
        void Unregist( const MGEN::Type::UnitID unit_id );
        void AddCount( const MGEN::Type::UnitID unit_id );

    private:
        void   InferenceCounterThreadRunner( void );
        void   InferenceCounterThreadCloser( void );

    private:
        std::unordered_map<MGEN::Type::UnitID, std::atomic<unsigned int>> counters_;
        MGEN::SafeThread        thread_;
        mutable std::mutex      mutex_;
        std::condition_variable cond_;
    };

} // namespace MGEN
