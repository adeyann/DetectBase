#pragma once

#include "MgenTypes.h"
#include "json/json_fwd.hpp"

#include <set>
#include <unordered_map>
#include <string>

namespace MGEN
{
    // --- Domain Specific Type Aliases ---
    using DeviceIDSet      = std::set<MGEN::Type::DeviceID>;
    using CameraIDSet      = DeviceIDSet;

    // DETECTOR 분기 전용 단일 카메라 클러스터.
    class CameraCluster_DETECTOR
    {
    public:
        // setter use json
        bool AddClusterData( const nlohmann::json& json, const int identifier = 0 );

        // setter json key value
        void SetDeviceKeyname( const std::string& key );

        // clear all cluster data
        void ClearAll( void );

        // getter : set<device id>
        DeviceIDSet GetDeviceSet( void ) const;

    private:
        CameraIDSet camera_set_;
        std::string camera_key_name_;
    };

} // namespace MGEN
