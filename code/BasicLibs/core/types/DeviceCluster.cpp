#include "DeviceCluster.h"
#include "json/json.hpp"
#include "MgenLogger.h"

namespace MGEN
{
    namespace
    {
        constexpr int JSON_PARSE_ID_VALUE_NOT_SET = -1;
    }

    void CameraCluster_DETECTOR::SetDeviceKeyname( const std::string& key )
    {
        this->camera_key_name_ = key;
    }

    bool CameraCluster_DETECTOR::AddClusterData( const nlohmann::json& json, const int identifier )
    {
        if( this->camera_key_name_.empty() ) {
            MLOG_ERROR("%s() -> JSON camera parsing key name is not set",
                __func__ );
            return false;
        }

        if( json.is_array() == false ) {
            MLOG_ERROR("%s() -> JSON file not array type : \n%s\n",
                __func__, json.dump(2).c_str() );
            return false;
        }

        if( identifier != 0 ) {
            // just warning
            MLOG_WARN("%s() -> undefined behavior, identifier(%d) not set default(0)", __func__, identifier );
        }

        for( const auto& object : json ) {
            if( object.is_object() && object.contains( this->camera_key_name_ ) )
                this->camera_set_.insert( object.value( this->camera_key_name_, JSON_PARSE_ID_VALUE_NOT_SET ) );
        }

        if( this->camera_set_.count( JSON_PARSE_ID_VALUE_NOT_SET ) > 0 )
            this->camera_set_.erase( JSON_PARSE_ID_VALUE_NOT_SET );

        return true;
    }

    void CameraCluster_DETECTOR::ClearAll( void )
    {
        this->camera_set_.clear();
    }

    DeviceIDSet CameraCluster_DETECTOR::GetDeviceSet( void ) const
    {
        return this->camera_set_;
    }


} // namespace MGEN
