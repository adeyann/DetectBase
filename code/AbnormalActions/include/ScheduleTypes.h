#pragma once

/**
 * Declaration and definition header file
 * for AbnormalActions(Events) Schedule
 */

// STL :: C++
#include <vector>
#include <deque>
#include <map>

// BasicLibs
#include "InferObject.h"        // MGEN::InferObject
#include "AbnormalEventTypes.h" // MGEN::Abnormal::EventClass
#include "MgenTypes.h"          // MGEN::EventTime

// Management
#include "SettingData.h"

namespace MGEN::Abnormal
{
    using DataLayer    = std::vector<MGEN::InferObject>;
    using ScheduleInfo = ScheduleSettingData::AbnormalEventScheduleInfo;

    // For IntrusionZone (AreaIntrusion)
    struct  RoiEventInfo
    {
        bool      continuous = false;
        EventTime event_time = std::chrono::system_clock::now();
    };

    // For VehicleParking
    struct LoiteringEventInfo
    {
        bool      is_history_compressed    = false;
        EventTime last_compressed_end_time = std::chrono::system_clock::now();
        std::vector<std::pair<MGEN::InferObject, EventTime>> infer_objects;
    };

} // namespace MGEN::Abnormal