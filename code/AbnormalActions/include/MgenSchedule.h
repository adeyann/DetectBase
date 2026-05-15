#pragma once

// STL :: C++
#include <unordered_map>
#include <functional>
#include <utility> // pair
#include <memory>
#include <vector>

// OpenCV
#include <opencv2/opencv.hpp>
#include <opencv2/core.hpp>

// BasicLibs
#include "json/json_fwd.hpp"
#include "InferObject.h"
#include "TrackerBase/TrackerTypes.h"
#include "ClassChecker.h"

// AbnormalActions
#include "ScheduleTypes.h"
#include "HistoryChecker.h"
#include "AbnormalActionChecker.h"

namespace MGEN::Abnormal
{
	class Schedule
	{
	public:
		Schedule(
			const ScheduleInfo& info,
			const int frame_w, const int frame_h, const int limit_fps,
			MGEN::ClassEqualChecker class_equal_checker
		) noexcept;

		~Schedule() = default;

		// 실제로 이상행동 알고리즘 Validate하는 함수
		DataLayer Check( const DataLayer& in_layer ) noexcept;

		// Get & Set For Detector Level
		const int  GetUidSuffixNumber( const int track_id, const int non_track_suffix_identifier ) const noexcept;
		const bool IsEvent( const EventClass Event ) const noexcept { return sch_info.event_code == Event; }

		// Calculation For Abnormal Algorithm Process Level
		void         UpdateEventActivateTime( void ) noexcept;
		const bool   GetEnoughEventInterval( void ) const noexcept;
		const bool   IsWithinExcludeROI( const MGEN::InferObject& infer_object ) const noexcept;
		const int    GetDurationIfSatisfyEventRequireSeconds( EventTime event_start_time ) const noexcept;

	// Static Functions
	public:
		static std::string GetEventName( const EventClass event_type ) noexcept;
		static TrackFlag   GetTrackType( const EventClass event_type ) noexcept;

		// extend_data를 분석하여 JSON에 Key-Value를 주입하는 정적 메서드
    	static void SetExtendedDataToJson(
			nlohmann::json& json_object, const MGEN::InferObject& infer_object, const EventClass event_type ) noexcept;

	private:
		// initialize function
		void SelectChecker( const EventClass event_type ) noexcept;

		const bool SetClassEqualChecker( MGEN::ClassEqualChecker cec );
		const int  GetAbnormalRequireFrame( const EventClass event_type ) const noexcept;
		const int  GetAbnormalReleaseFrame( const EventClass event_type ) const noexcept;

		std::vector<std::vector<cv::Point>>
		MakeCvScaledRoiFromScheduleRoi( const MGEN::ROI<float>& rois, const int frame_w, const int frame_h ) const noexcept;

		// check exceptions
		const bool IsWithinBlackList( const std::vector<std::string>& black_list ) const noexcept;
		const bool IsWithinWeeklyDay( const std::vector<int>& weekly_list ) const noexcept;
		const bool IsWithinTimeRange( const std::string& startTime, const int rangeInMinute ) const;

	public:
		// Const inited when constructor
		const ScheduleInfo sch_info;
		const int          abnormal_require_seconds;
		const int          abnormal_release_seconds;

		// roi_points vectors
		const std::vector<std::vector<cv::Point>> cv_rois;
		const std::vector<std::vector<cv::Point>> cv_exclude_rois;
		const std::vector<std::vector<cv::Point>> cv_two_way_rois; // 양방향 경계선
		const std::vector<std::vector<cv::Point>> cv_one_way_rois; // 단방향 경계선

		// HistoryChecker classes
		HistoryChecker<std::vector<MGEN::InferObject>> object_history;      	// LineIntrusion, VehicleIntrusion
		HistoryChecker<RoiEventInfo>			       roi_state_history;   	// AreaIntrusion
		HistoryChecker<LoiteringEventInfo>			   moving_state_history;   	// VehicleParking

		// class equal checker
		MGEN::ClassEqualChecker class_equal_checker = nullptr;

	private:
		EventTime    last_active_time;
		EventChecker checker;

	}; // cls : Schedule

} // namespace MGEN::Abnormal
