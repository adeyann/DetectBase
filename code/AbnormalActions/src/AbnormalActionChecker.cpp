// BasicLibs
#include "MgenLogger.h"

// AbnormalActions
#include "AbnormalActionChecker.h"
#include "MgenSchedule.h"
#include "GeometricLogic.h"

#include <limits>
#include <cmath>
#include <algorithm>

namespace MGEN::Abnormal // MgenSolution's abnormal event namespace
{
    using namespace std;
    using namespace std::chrono_literals;

    namespace
    {
		constexpr size_t DATA_LAYER_HIST_MAX        = 600;
		constexpr size_t REQUIRE_MIN_TRACKING_COUNT = 10;
    }

    // Declare inner static functions
    static DataLayer LineIntrusionBase ( const DataLayer& in_layer, Schedule* const schedule, const std::string class_name ) noexcept;


    DataLayer VehicleParking( const DataLayer& in_layer, Schedule* const schedule ) noexcept
    {
        DataLayer results{};

        if( schedule->class_equal_checker == nullptr )
            return results;

        auto* const target_history = &( schedule->moving_state_history );

        // Detect : Parking
        for( const auto& object : in_layer )
        {
            // Only worrks target class
            if( schedule->class_equal_checker( object.class_id, "Car" ) == false )
                continue;

            // Check Exclude ROI
            if( schedule->IsWithinExcludeROI( object ) )
                continue;

            bool is_exist_roi_contain_box = false;
            for( const auto& roi_points : schedule->cv_rois ) {
                if( isIncludePoint( getMidPointFromInferObject( object ), roi_points ) )
                    is_exist_roi_contain_box = true;
            }

            // Get history by target track_id
            auto& history_objects = target_history->GetHistory( object.track_id ).infer_objects;

            if( is_exist_roi_contain_box )
            {
                // Update history
                history_objects.push_back( std::make_pair( object, std::chrono::system_clock::now() ) );

                // clear if over threshold
                if( history_objects.size() > DATA_LAYER_HIST_MAX )
                {
                    const auto back_up_object = history_objects.front();
                    history_objects.clear();
                    history_objects.push_back( std::move( back_up_object ) );
                }
            }
            // 현재 프레임에서 검출되지 않았다면 굳이 아래로 갈 필요가...?
            else { continue; }

            if( history_objects.size() < REQUIRE_MIN_TRACKING_COUNT )
                continue;

            // get first tracked data
            // NEW-11: first_tracked_object 사용 안 됨 → throwaway 로 변경
            const auto& [ _, first_tracked_time ] = history_objects.front();

            // Check Loitering
            const int duration_if_enough_interval = schedule->GetDurationIfSatisfyEventRequireSeconds( first_tracked_time );

            if( ( duration_if_enough_interval > 0 ) && schedule->GetEnoughEventInterval() )
            {
                MGEN::InferObject parking_object = object;
                parking_object.extend_data.push_back( duration_if_enough_interval ); // Duration

                results.push_back( std::move(parking_object) );
            }
        }

        // Update Interval Timer
        if (results.size() > 0)
            schedule->UpdateEventActivateTime();

        return results;
    }

    static const Abnormal::LineOrientation calcLineCross( const DataLayer& target_history, const vector<vector<cv::Point>>& target_rois ) noexcept
    {
        // Per each Roi lines
        for( const auto& roi_points : target_rois ) {
            // Per each Intrusion Line
            for( size_t curr_line_idx = 0; curr_line_idx < roi_points.size() - 1; ++curr_line_idx ) {
                // target_history 의 각 Point 들에 대해 직전 Point 이후 진행에서 IntrusionLine 지나쳤는지
                for( size_t curr_hist_idx = 0; curr_hist_idx < target_history.size() - 1; ++curr_hist_idx ) {

                    const size_t next_line_idx = curr_line_idx + 1;
                    const size_t next_hist_idx = curr_hist_idx + 1;

                    const auto& prev_track_box = target_history.at( curr_hist_idx );
                    const auto& next_track_box = target_history.at( next_hist_idx );

                    const auto  prev_cv_point = getMidPointFromInferObject( prev_track_box );
                    const auto  next_cv_point = getMidPointFromInferObject( next_track_box );

                    const auto line_cross = checkLineIntersect( prev_cv_point, next_cv_point, roi_points.at( curr_line_idx ), roi_points.at( next_line_idx ) );
                    if( line_cross != Abnormal::LineOrientation::NotCrossed )
                        return line_cross;
                }
            }
        }
        return Abnormal::LineOrientation::NotCrossed;
    }

    DataLayer LineIntrusionBase( const DataLayer& in_layer, Schedule* const schedule, const std::string class_name ) noexcept
    {
    	DataLayer results {};

        if( schedule->class_equal_checker == nullptr )
            return results;

        auto* const target_history = &( schedule->object_history );

        target_history->ReleaseInactiveHistory( in_layer );

        // Detect : Line Intrusion
        for( const auto& object : in_layer )
        {
            // Only works target class_type
            if( schedule->class_equal_checker( object.class_id, class_name ) == false )
                continue;

            // Check Exclude ROI
            if( schedule->IsWithinExcludeROI( object ) )
                continue;

            // Get history by target track_id
            auto& history_boxes = target_history->GetHistory( object.track_id );

            // Update history
            history_boxes.push_back( object );

            // 만약 정지된 객체를 계속 잡고 있으면 무한정 쌓일 수 있으니 MAX값 이상이면 초기화
            if( history_boxes.size() > DATA_LAYER_HIST_MAX )
                history_boxes.clear();

            // 계속 집어넣다가 일정 Frame 이상 쌓였을 때만 체크 : 끊어져서 소규모로 생성되는 TrackID에 과민반응하지 않기 위해
            if( history_boxes.size() < REQUIRE_MIN_TRACKING_COUNT )
                continue;

            if( schedule->GetEnoughEventInterval() == false )
                continue;

            Abnormal::LineOrientation line_direction = Abnormal::LineOrientation::NotCrossed;

            // DETECT => Two-way
            const auto line_cross_two_way = calcLineCross( history_boxes, schedule->cv_two_way_rois );
            if( line_cross_two_way != Abnormal::LineOrientation::NotCrossed )
                line_direction = line_cross_two_way;

            // DETECT => One-way (clockwise)
            const auto line_cross_one_way = calcLineCross( history_boxes, schedule->cv_one_way_rois );
            if( line_cross_one_way == Abnormal::LineOrientation::Clockwise )
                line_direction = line_cross_one_way;

            // Check Exist Line Cross
            if( line_direction != Abnormal::LineOrientation::NotCrossed )
            {
                // Update ClassID to AbnormalEventID
                MGEN::InferObject cross_box  = object;
                // Direction
                cross_box.extend_data.push_back( static_cast<int>( line_direction ) * 1.0f ); // Direction

                // Add result
                results.push_back( std::move( cross_box ) );
            }
        }

        // Update Interval Timer
        if( results.size() > 0 )
            schedule->UpdateEventActivateTime();

        return results;
    }

    DataLayer IntrusionLine( const DataLayer& in_layer, Schedule* const schedule ) noexcept
    {
        return LineIntrusionBase( in_layer, schedule, std::string { "Person" } );
    }

    DataLayer VehicleIntrusion( const DataLayer& in_layer, Schedule* const schedule ) noexcept
    {
        return LineIntrusionBase( in_layer, schedule, std::string { "Car" } );
    }

    DataLayer IntrusionZone( const DataLayer& in_layer, Schedule* const schedule ) noexcept
    {
        DataLayer results{};

        if( schedule->class_equal_checker == nullptr )
            return results;

        auto* const target_history = &( schedule->roi_state_history );

        target_history->ReleaseInactiveHistory(in_layer);

        // Detect : Intrusion Zone
        for( const auto& object : in_layer )
        {
            // Only worrks target class
            if( schedule->class_equal_checker( object.class_id, "Person" ) == false )
                continue;

            // Check Exclude ROI
            if( schedule->IsWithinExcludeROI( object ) )
                continue;

            bool is_exist_roi_contain_box = false;
            for( const auto& roi_points : schedule->cv_rois ) {
                if( isIncludePoint( getMidPointFromInferObject( object ), roi_points ) )
                    is_exist_roi_contain_box = true;
            }

            // Get history by target track_id
            auto& roi_check = target_history->GetHistory( object.track_id );

            if( is_exist_roi_contain_box == false ){
                roi_check.continuous = true;
                roi_check.event_time = chrono::system_clock::now();
            }

            if( schedule->GetEnoughEventInterval() == false )
                continue;

            for( const auto& roi_points : schedule->cv_rois ) {
                // 영역 바깥에 있었던 적이 최소 한 번 있으면서 현재는 영역 내부일 때
                if( roi_check.continuous == true && isIncludePoint( getMidPointFromInferObject( object ), roi_points ) ) { // => DETECTED
                    // Check target box already exist at results
                    auto it = find_if( results.begin(), results.end(),
                                [&object] ( const MGEN::InferObject& elem ) {
                                    return elem.track_id == object.track_id;
                                } );

                    if( it != results.end() )
                        continue;

                    // get duration
                    const auto now_time = chrono::system_clock::now();
                    const auto duration = chrono::duration_cast<chrono::seconds>( now_time - roi_check.event_time );

                    // Update ClassID to AbnormalEventID
                    MGEN::InferObject Intrusion_object = object;
                    Intrusion_object.extend_data.push_back( duration.count() ); // Duration

                    // Add Result
                    results.push_back( std::move( Intrusion_object ) );
                }
            }
        }

        // Update Interval Timer
        if( results.size() > 0 )
            schedule->UpdateEventActivateTime();

        return results;
    }

};
