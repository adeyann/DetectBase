#pragma once

// STL :: C++
#include <unordered_map> // std::unordered_map
#include <utility>       // std::pair

// BasicLibs
#include "InferObject.h"

// AbnormalActions
#include "ScheduleTypes.h"

namespace MGEN::Abnormal
{
	// When checking abnormal behavior algorithms,
	// a class that manages the data necessary for
	// algorithm checking for each tracking ID as history.
	template <typename T>
	class HistoryChecker
	{
	public:
		// Constructor to initialize the history_map
		HistoryChecker( const int release_limit_secs )
            : release_threshold_millisecs( release_limit_secs * 1000 )
		{
            //
        }

		// Get history by track_id
		T& GetHistory( const InferTrackID track_id ) noexcept
		{
			auto iter = history_map.find( track_id );
			if( iter == history_map.end() )
			{
				this->CreateNewHistory( track_id );
				iter = history_map.find( track_id );
			}
			else
			{
				// When a track_id from a significant time ago coincides with an IOU,
				// check the time and override the corresponding history information.
				const auto now_time = MonotonicClock::now();
				const auto duration = std::chrono::duration_cast<std::chrono::milliseconds>( now_time - iter->second.second );
				if( duration.count() > this->release_threshold_millisecs ){
					this->OverrideHistory( track_id );
					iter = history_map.find( track_id );
				}
			}

			return iter->second.first;
		}

		// Release history that has been inactive for more than a certain period (release_threshold_secs)
		// @param check_target_objects : vector of objects tracked at the current
		void ReleaseInactiveHistory( const DataLayer& check_target_objects ) noexcept
		{
			for( auto hist_iter = history_map.begin(); hist_iter != history_map.end(); )
            {
				// Get target history track id
				const InferTrackID target_history_track_id = hist_iter->first;

				// find iterator such that equal track id
				const auto track_object_iter =
					std::find_if( check_target_objects.begin(), check_target_objects.end(),
                        [=]( const MGEN::InferObject& check_object ){
                            return check_object.track_id == target_history_track_id;
                        }
                    );

				// If TrackID is in History but not in check_target_objects, release_count++
				auto& target_history_release_time = ( hist_iter->second ).second;

				bool is_exist_track_history = false;
				if( track_object_iter != check_target_objects.end() ) {
					is_exist_track_history = true;
				}

				const auto now_time = MonotonicClock::now();
				const auto duration = std::chrono::duration_cast<std::chrono::milliseconds>( now_time - target_history_release_time );

				// Delete history that exceeds the release limit
				if( duration.count() >= this->release_threshold_millisecs ) {
					hist_iter = history_map.erase( hist_iter );
				}
				else {
					if( is_exist_track_history == true ) {
						target_history_release_time = MonotonicClock::now();
					}
					++hist_iter;
				}
			}
		}

	private:
		// duration 측정 전용 monotonic clock — NTP jump/wall-time 변경 영향 없음
		using MonotonicClock = std::chrono::steady_clock;

		// create new history about track_id
		void CreateNewHistory( const InferTrackID track_id ) noexcept
		{
			if( history_map.find( track_id ) == history_map.end() )
				history_map[track_id] = std::make_pair( T {}, MonotonicClock::now() );
		}
		// override history about track_id
		void OverrideHistory( const InferTrackID track_id ) noexcept
		{
			if( history_map.find( track_id ) != history_map.end() )
				history_map[track_id] = std::make_pair( T {}, MonotonicClock::now() );
		}

	private:
		std::unordered_map<InferTrackID, std::pair<T, MonotonicClock::time_point>> history_map;
		const int release_threshold_millisecs;
	}; // cls : HistoryChecker

} // namespace MGEN::Abnormal
