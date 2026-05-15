#pragma once

// STL :: C++
#include <functional> // std::function

// AbnormalActions
#include "ScheduleTypes.h" // DataLayer

// Mgensolution's default namespace
namespace MGEN::Abnormal
{
    // Declare
    class Schedule;
    using EventChecker = std::function<DataLayer( const DataLayer&, Schedule* const )>;

    // Algorithm check (사람 + 차량 × 라인 + 영역)
    DataLayer IntrusionLine    ( const DataLayer& in_layer, Schedule* const schedule_info ) noexcept;
    DataLayer IntrusionZone    ( const DataLayer& in_layer, Schedule* const schedule_info ) noexcept;
    DataLayer VehicleIntrusion ( const DataLayer& in_layer, Schedule* const schedule_info ) noexcept;
    DataLayer VehicleParking   ( const DataLayer& in_layer, Schedule* const schedule_info ) noexcept;

}; // namespace MGEN::Abnormal