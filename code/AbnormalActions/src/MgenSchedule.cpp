//
#include "MgenSchedule.h"
#include "GeometricLogic.h"

// BasicLibs
#include "json/json.hpp"
#include "MgenLogger.h"

namespace MGEN::Abnormal
{
    using namespace std;

    Schedule::Schedule( const ScheduleInfo& info, const int frame_w, const int frame_h, const int /*limit_fps*/, MGEN::ClassEqualChecker cec ) noexcept
        : sch_info                ( info )
        , abnormal_require_seconds( GetAbnormalRequireFrame( info.event_code ) )
        , abnormal_release_seconds( GetAbnormalReleaseFrame( info.event_code ) )
        , cv_rois                 ( MakeCvScaledRoiFromScheduleRoi( info.roi, frame_w, frame_h ) )
        , cv_exclude_rois         ( MakeCvScaledRoiFromScheduleRoi( info.exclude_roi, frame_w, frame_h ) )
        , cv_two_way_rois         ( MakeCvScaledRoiFromScheduleRoi( info.two_way_roi, frame_w, frame_h ) )
        , cv_one_way_rois         ( MakeCvScaledRoiFromScheduleRoi( info.one_way_roi, frame_w, frame_h ) )
        , object_history          ( abnormal_release_seconds )
        , roi_state_history       ( abnormal_release_seconds )
        , moving_state_history    ( abnormal_release_seconds )
        , last_active_time        ( chrono::system_clock::now() )
    {
        SelectChecker( this->sch_info.event_code );

        if( this->SetClassEqualChecker( std::move( cec ) ) == false ){
            MLOG_WARN("Schedule { %s } set class equal checker failed.", this->GetEventName(info.event_code).c_str() );
        }
    }

    DataLayer Schedule::Check( const DataLayer& in_layer ) noexcept
    {
        if( IsWithinBlackList( this->sch_info.black_list_days ) )
            return DataLayer {};

        if( !IsWithinWeeklyDay( this->sch_info.weekly ) )
            return DataLayer {};

        if( !IsWithinTimeRange( this->sch_info.start_time, this->sch_info.range_minutes ) )
            return DataLayer {};

        // Run checker() function
        if( this->checker )
            return this->checker( in_layer, this );

        return DataLayer {};
    }

    const bool Schedule::IsWithinExcludeROI( const MGEN::InferObject& infer_object ) const noexcept
    {
        if( this->cv_exclude_rois.size() == 0 )
            return false;

        // check each exclude roi
        for( auto& exclude_roi : this->cv_exclude_rois )
            if( isIncludePoint( getMidPointFromInferObject( infer_object ), exclude_roi ) == true )
                return true;
        return false;
    }

    void Schedule::SelectChecker( const EventClass event_type ) noexcept
    {
        switch( event_type ) {
        case EventClass::LineIntrusion:    this->checker = Abnormal::IntrusionLine;    break;
        case EventClass::AreaIntrusion:    this->checker = Abnormal::IntrusionZone;    break;
        case EventClass::VehicleIntrusion: this->checker = Abnormal::VehicleIntrusion; break;
        case EventClass::VehicleParking:   this->checker = Abnormal::VehicleParking;   break;
        default:                           this->checker = nullptr;                    break;
        }
    }

    const bool Schedule::SetClassEqualChecker( MGEN::ClassEqualChecker cec )
    {
        if( !cec )
            return false;

        this->class_equal_checker = std::move( cec );
        return true;
    }

    const int Schedule::GetAbnormalRequireFrame( const EventClass event_type ) const noexcept
    {
        switch( event_type ) {
        case EventClass::VehicleParking: return this->sch_info.loitering_require_dur_sec;
        case EventClass::LineIntrusion:  return MGEN::DefineDefault::DEFAULT_EVENT_REQUIR_DURATION_SEC;
        case EventClass::AreaIntrusion:  return MGEN::DefineDefault::DEFAULT_EVENT_REQUIR_DURATION_SEC;
        default:                         return MGEN::DefineDefault::DEFAULT_EVENT_REQUIR_DURATION_SEC;
        }
    }

    const int Schedule::GetAbnormalReleaseFrame( const EventClass event_type ) const noexcept
    {
        switch( event_type ) {
        case EventClass::VehicleParking: return MGEN::DefineDefault::DEFAULT_EVENT_FRAME_RELEASE_L_SEC;
        case EventClass::LineIntrusion:  return MGEN::DefineDefault::DEFAULT_EVENT_FRAME_RELEASE_S_SEC;
        case EventClass::AreaIntrusion:  return MGEN::DefineDefault::DEFAULT_EVENT_FRAME_RELEASE_S_SEC;
        default:                         return MGEN::DefineDefault::DEFAULT_EVENT_FRAME_RELEASE_S_SEC;
        }
    }

    string Schedule::GetEventName( const EventClass event_type ) noexcept
    {
        switch( event_type ) {
        case EventClass::LineIntrusion:    return string { "LineIntrusion" };
        case EventClass::AreaIntrusion:    return string { "AreaIntrusion" };
        case EventClass::VehicleIntrusion: return string { "VehicleIntrusion" };
        case EventClass::VehicleParking:   return string { "VehicleParking" };
        default:                           return string { "NotDefined" };
        }
    }

    TrackFlag Schedule::GetTrackType( const EventClass event_type ) noexcept
    {
        switch( event_type ) {
        case EventClass::LineIntrusion:    return TRACK_ON_PERSON;
        case EventClass::AreaIntrusion:    return TRACK_ON_PERSON;
        case EventClass::VehicleIntrusion: return TRACK_ON_CAR;
        case EventClass::VehicleParking:   return TRACK_ON_CAR;
        default:                           return TRACK_ALL_OFF;
        }
    }

    void Schedule::SetExtendedDataToJson( nlohmann::json& json_object, const MGEN::InferObject& infer_object, const EventClass event_type ) noexcept
    {
        // 데이터가 없으면 주입하지 않음
        if( infer_object.extend_data.empty() )
            return;

        // extend_data는 float vector이므로 필요 시 int로 캐스팅하여 사용
        const auto val = infer_object.extend_data.front();

        switch( event_type )
        {
        case EventClass::LineIntrusion:
            // "Direction" : 침입 방향 (LineOrientation)
            // 0: OnLine, 1: Clockwise, 2: CounterClockwise, 3: NotCrossed
            json_object["Direction"] = static_cast<int>( val );
            break;

        case EventClass::AreaIntrusion:
        case EventClass::VehicleParking:
            // "Duration" : 머문 시간 (초)
            json_object["Duration"] = static_cast<int>( val );
            break;

        default:
            // 그 외 이벤트는 extend_data를 사용하지 않거나 정의되지 않음
            break;
        }
    }

    const int Schedule::GetUidSuffixNumber( const int track_id, const int non_track_suffix_identifier ) const noexcept
    {
        switch( this->sch_info.event_code ) {
        case EventClass::AreaIntrusion:
            return ( track_id == NON_TRACKING_IDX ) ? 0 : track_id;
        default:
            return non_track_suffix_identifier;
        }
    }

    void Schedule::UpdateEventActivateTime( void ) noexcept
    {
        this->last_active_time = chrono::system_clock::now();
    }

    // 같은 스케쥴 내에서 event notify를 위한 최소 인터벌
    const bool Schedule::GetEnoughEventInterval( void ) const noexcept
    {
        const auto now_time = chrono::system_clock::now();
        const auto duration = chrono::duration_cast<chrono::milliseconds>( now_time - this->last_active_time );
        const int  interval = this->sch_info.notification_min_interval_sec * 1000;
        return ( duration.count() > interval );
    }

    // 같은 history에서 event 발생을 위해 필요한 최소 duration
    const int Schedule::GetDurationIfSatisfyEventRequireSeconds( EventTime event_start_time ) const noexcept
    {
        const auto now_time = chrono::system_clock::now();
        const auto duration = chrono::duration_cast<chrono::milliseconds>( now_time - event_start_time );
        const int  interval = this->abnormal_require_seconds * 1000;
        return ( ( duration.count() > interval ) ? static_cast<int>( duration.count() / 1000 ) : 0 );
    }

    const bool Schedule::IsWithinBlackList( const vector<string>& black_list ) const noexcept
    {
        if( black_list.empty() )
            return false;

        struct tm tstruct {};
        auto      curr_time = std::time( nullptr );
        auto*     time_info = localtime_r( &curr_time, &tstruct );

        // Time Info Formatting
        char fmt_date[20] = { 0x00, };
        std::strftime( fmt_date, sizeof( fmt_date ), "%Y-%m-%d", time_info );
        const string str_date { fmt_date };

        for( const string& dateString : black_list ) {
            if( dateString == str_date )
                return true;
        }
        return false;
    }

    const bool Schedule::IsWithinWeeklyDay( const vector<int>& weekly_list ) const noexcept
    {
        if( weekly_list.size() != MGEN::DefineDefault::DAY_ABOUT_1_WEEK ) {
            MLOG_WARN( "Schedule:Weekly is invalid, size = %d", static_cast<int>( weekly_list.size() ) );
            return false;
        }

        struct tm curr_time_info {};
        auto curr_time = std::time( nullptr );
        localtime_r( &curr_time, &curr_time_info );

        return ( weekly_list[curr_time_info.tm_wday] == MGEN::DefineDefault::WEEKLY_DAY_ON );
    }

    const bool Schedule::IsWithinTimeRange( const string& startTime, const int rangeInMinute ) const
    {
        const int total_day_minutes = MGEN::DefineDefault::MIN_ABOUT_1_DAY;
        if( rangeInMinute >= total_day_minutes )
            return true;

        // parsing start time
        int targetHour, targetMin;
        if( sscanf( startTime.c_str(), "%2d:%2d", &targetHour, &targetMin ) != 2 ) {
            // parsing fail
            return false;
        }

        // set compare start time
        struct tm start_time_info {};
        start_time_info.tm_hour = targetHour;
        start_time_info.tm_min  = targetMin;
        start_time_info.tm_sec  = 0;

        // get current time
        struct tm curr_time_info {};
        auto curr_system_time = std::time( nullptr );
        localtime_r( &curr_system_time, &curr_time_info );

        struct tm filtered_curr_time_info {};
        filtered_curr_time_info.tm_hour = curr_time_info.tm_hour;
        filtered_curr_time_info.tm_min  = curr_time_info.tm_min;
        filtered_curr_time_info.tm_sec  = 0;

        // formatting
        const auto tp_start_time   = chrono::system_clock::from_time_t( std::mktime( &start_time_info ) );
        const auto tp_current_time = chrono::system_clock::from_time_t( std::mktime( &filtered_curr_time_info ) );

        // get minute diff
        const auto duration     = chrono::duration_cast<chrono::minutes>( tp_current_time - tp_start_time );
        const int  dur_count    = static_cast<int>( duration.count() );
        const int  minutes_diff = ( dur_count > 0 ) ? dur_count : dur_count + total_day_minutes;

        return ( minutes_diff <= rangeInMinute );
    }

    vector<vector<cv::Point>> Schedule::MakeCvScaledRoiFromScheduleRoi( const MGEN::ROI<float>& rois, const int frame_w, const int frame_h ) const noexcept
    {
        vector<vector<cv::Point>> results {};
        for( const auto& roi_points : rois ) {
            vector<cv::Point> scaled_roi {};
            scaled_roi.reserve( roi_points.size() );
            for( const auto& [xpos_ratio, ypos_ratio] : roi_points ) {
                scaled_roi.emplace_back(
                    clamp( 0, static_cast<int>( std::round( static_cast<float>( frame_w ) * xpos_ratio ) ), frame_w ),
                    clamp( 0, static_cast<int>( std::round( static_cast<float>( frame_h ) * ypos_ratio ) ), frame_h )
                );
            }
            results.push_back( std::move( scaled_roi ) );
        }
        return results;
    }

} // namespace MGEN::Abnormal

