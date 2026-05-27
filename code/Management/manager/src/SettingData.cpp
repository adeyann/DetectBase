#include "SettingData.h"
#include "MgenLogger.h"
#include "json/json.hpp"
#include "string_utils.h"

#include <functional>
#include <algorithm>

namespace MGEN
{
    static std::string GetStringRegardlessOfCase( const nlohmann::json& json_object, const std::string& key )
    {
        // Check input key case : camelCase, PascalCase, snake_case
        std::vector<std::string> possible_keys = {
            key,                        // Original key
            ConvertToCamelCase ( key ), // camelCase
            ConvertToPascalCase( key ), // PascalCase
            ConvertToSnakeCase ( key ), // snake_case
        };

        for( const auto& possible_key : possible_keys ) {
            if( json_object.contains( possible_key ) ) {
                return possible_key;
            }
        }

        return key; // Return original key if no match found
    }

    static bool Impl_ServerSettingData_UpdateFromJsonObject( ISettingData* interface_ptr, const nlohmann::json& json_object )
    {
        if( json_object.is_object() == false )
            return false;

        if( json_object.contains("Id") == false )
            return false;

        if( json_object.contains("Setting") == false )
            return false;

        // cast
        ServerSettingData* setting = dynamic_cast<ServerSettingData*>( interface_ptr );

        // parse id
        setting->service_server_id = json_object.value( "Id", 0 );

        // parse setting
        const nlohmann::json  empty_object = nlohmann::json::object();
        const nlohmann::json& setting_js   = json_object["Setting"].is_null() ? empty_object : json_object["Setting"];

        // set data
        setting->inference_per_cams_fps_limit = setting_js.value( "InferencePerCamsFpsLimit",  MGEN::DefineDefault::DFPS_LIMIT_PER_UNIT );
        setting->tracking_memory_object_limit = setting_js.value( "TrackingMemoryObjectLimit", MGEN::DefineDefault::TRACKING_OBJECT_HIST_LIMIT );
        setting->rtsp_proxies_sync_delay_ms   = setting_js.value( "RtspProxiesSyncDelayMs",    MGEN::DefineDefault::RTSP_PROXY_SYNC_DELAY_MS );
        setting->rtsp_proxy_publish_port      = setting_js.value( "RtspPort",                  MGEN::DefineDefault::RTSP_PROXY_PORT );
        setting->full_image_save_root_path    = setting_js.value( "FullImageSaveRootPath",     MGEN::DefineDefault::FULL_FRAME_IMAGE_SAVE_ROOT_PATH );
        setting->crop_image_save_root_path    = setting_js.value( "CropImageSaveRootPath",     MGEN::DefineDefault::CROP_FRAME_IMAGE_SAVE_ROOT_PATH );

        // 디버깅용 가상 boundary toggle — 키 누락 시 false (production-safe default).
        setting->debug_virtual_lines_enabled  = setting_js.value( "DebugVirtualLinesEnabled",  false );

        // for legacy: 옛 키 "DetectFps" -> "InferencePerCamsFpsLimit"
        if( setting_js.contains( "InferencePerCamsFpsLimit" ) == false && setting_js.contains( "DetectFps" ) == true ) {
            setting->inference_per_cams_fps_limit = setting_js.value( "DetectFps", MGEN::DefineDefault::DFPS_LIMIT_PER_UNIT );
        }

#if defined(__SANITIZE_THREAD__)
        // TSan build 전용: 100x slowdown 환경에서 packet drop / hang 방지 위해 fps=1 강제.
        // race detection 은 매 frame thread interaction 에서 즉시 stderr 출력되므로 1 fps 로도 충분.
        // production / ASan 빌드는 영향 0 (compile-time guard).
        setting->inference_per_cams_fps_limit = 1;
#endif

        return true;
    }

    static bool Impl_CameraSettingData_UpdateFromJsonObject( ISettingData* interface_ptr, const nlohmann::json& json_object )
    {
        if( json_object.is_object() == false )
            return false;

        // cast
        CameraSettingData* setting = dynamic_cast<CameraSettingData*>( interface_ptr );

        const std::string camera_id_key   = GetStringRegardlessOfCase( json_object, "CameraId" );
        const std::string url_key         = GetStringRegardlessOfCase( json_object, "Url" );
        const std::string access_ip_key   = GetStringRegardlessOfCase( json_object, "AccessIp" );
        const std::string access_id_key   = GetStringRegardlessOfCase( json_object, "AccessId" );
        const std::string access_pw_key   = GetStringRegardlessOfCase( json_object, "AccessPassword" );
        const std::string extend_data_key = GetStringRegardlessOfCase( json_object, "ExtendData" );

        // set data
        setting->camera_id   = json_object.value( camera_id_key,   0 );
        setting->url         = json_object.value( url_key,         "None" );
        setting->access_ip   = json_object.value( access_ip_key,   "None" );
        setting->access_id   = json_object.value( access_id_key,   "None" );
        setting->access_pw   = json_object.value( access_pw_key,   "None" );
        setting->extend_data = json_object.value( extend_data_key, nlohmann::json::object() );

        // Extend : SurveyData
        const std::string survey_data_key = GetStringRegardlessOfCase( json_object, "SurveyData" );
        if( json_object.contains( survey_data_key ) )
        {
            const nlohmann::json  empty_object   = nlohmann::json::object();
            const nlohmann::json& survey_data_js = json_object[survey_data_key].is_null() ? empty_object : json_object[survey_data_key];

            // Set extend data : SurveyData
            if( survey_data_js.empty() == false ) { setting->extend_data = survey_data_js; }
        }

        // Extend : Setting/ROI
        const std::string setting_key = GetStringRegardlessOfCase( json_object, "setting" );
        const std::string roi_key     = GetStringRegardlessOfCase( json_object, "roi" );
        if( json_object.contains( setting_key ) )
        {
            const nlohmann::json  empty_object   = nlohmann::json::object();
            const nlohmann::json& setting_js     = json_object[setting_key].is_null() ? empty_object : json_object[setting_key];

            // Insert "roi" object to setting
            if( json_object.contains( roi_key ) )
            {
                const nlohmann::json& roi_js = json_object[roi_key].is_null() ? empty_object : json_object[roi_key];
                if( roi_js.empty() == false )
                {
                    // Copy existing setting
                    nlohmann::json setting_with_roi_js = setting_js;

                    // Insert roi object
                    setting_with_roi_js[roi_key] = roi_js;

                    // Set extend data with roi included
                    setting->extend_data = setting_with_roi_js;
                }
            }
            else
            {
                // If no separate "roi" object, just set the setting_js as extend data
                if( setting_js.empty() == false ) {
                    setting->extend_data = setting_js;
                }
            }
        }

        return true;
    }

    static bool Impl_ExcludeCamSettingData_UpdateFromJsonObject( ISettingData* interface_ptr, const nlohmann::json& json_object )
    {
        // cast
        ExcludeCamSettingData* setting = dynamic_cast<ExcludeCamSettingData*>( interface_ptr );

        if( json_object.empty() )
            setting->is_exclude = false;
        else
            setting->is_exclude = true;

        return true;
    }

    static bool Impl_ScheduleSettingData_UpdateFromJsonArray( ISettingData* interface_ptr, const nlohmann::json& json_array )
    {
        using namespace MGEN::DefineDefault;
        using nlohmann::json;

        if( json_array.is_array() == false )
            return false;

        // cast
        ScheduleSettingData* setting = dynamic_cast<ScheduleSettingData*>( interface_ptr );

        // 만약 빈 array라면 스케줄이 전부 삭제된 것 : 정상 시나리오!
        if( json_array.empty() ) {
            setting->schedules.clear();
            return true;
        }

        // for swap data
        std::vector<ScheduleSettingData::AbnormalEventScheduleInfo> updateSchedules;

        // 각 arrary 내 개별 json object 유효한지 검증 후 대입.
        // 실패한 schedule 은 명시적으로 skip + MLOG_WARN 으로 어느 항목이 결함인지 식별 가능.
        size_t skipped_count = 0;
        for( const auto& obj : json_array ) {
            // W-06: type 검증까지 추가 (.get<T>() throw 가능성 제거).
            if( obj.is_object() == false || obj.contains( "Id" ) == false || !obj["Id"].is_number_integer() ) {
                ++skipped_count;
                MLOG_WARN("ScheduleSettingData: schedule item 'Id' 누락 또는 정수 아님. skip.");
                continue;
            }

            const int schedule_id = obj["Id"].get<int>();

            // 필수 필드 검증 — 결함 위치를 명시적으로 식별
            if( !obj.contains("Data") || !obj["Data"].is_object() ) {
                ++skipped_count;
                MLOG_WARN("ScheduleSettingData: schedule_id=%d 'Data' 필드 누락. skip.", schedule_id);
                continue;
            }
            if( !obj["Data"].contains("Rule") || !obj["Data"]["Rule"].is_object() ) {
                ++skipped_count;
                MLOG_WARN("ScheduleSettingData: schedule_id=%d 'Data.Rule' 필드 누락. skip.", schedule_id);
                continue;
            }
            const auto& rule = obj["Data"]["Rule"];
            // W-06: EventCode / LevelCode type 검증 추가
            if( !rule.contains("EventCode") || !rule["EventCode"].is_number_integer() ||
                !rule.contains("LevelCode") || !rule["LevelCode"].is_number_integer() ) {
                ++skipped_count;
                MLOG_WARN("ScheduleSettingData: schedule_id=%d 'Rule.EventCode' or 'Rule.LevelCode' 누락 또는 정수 아님. skip.", schedule_id);
                continue;
            }
            if( !rule.contains("TimeRange") || !rule["TimeRange"].is_object() ) {
                ++skipped_count;
                MLOG_WARN("ScheduleSettingData: schedule_id=%d 'Rule.TimeRange' 누락. skip.", schedule_id);
                continue;
            }

            ScheduleSettingData::AbnormalEventScheduleInfo sch {};

            // Default data — 위 검증으로 안전 access 보장
            sch.schedule_id = schedule_id;
            sch.event_code  = static_cast<MGEN::Abnormal::EventClass>( rule["EventCode"].get<int>() );
            sch.level_code  = rule["LevelCode"].get<int>();

            // Time Range
            const auto& time_object  = rule["TimeRange"];
            sch.start_time    = time_object.value( "Start", std::string{ "00:00" } );
            sch.range_minutes = time_object.value( "Range", MIN_ABOUT_1_DAY );

            if( time_object.contains("Weekly") && time_object["Weekly"].is_array() ) {
                // W-06: 각 element type 검증 추가
                for( const auto& each_day : time_object["Weekly"] ) {
                    if( each_day.is_number_integer() )
                        sch.weekly.push_back( each_day.get<int>() );
                }
            } else {
                for( size_t day = 0; day < DAY_ABOUT_1_WEEK; ++day )
                    sch.weekly.push_back( WEEKLY_DAY_ON );
            }

            // ROI — 누락 시 빈 array 로 처리 (default 전체 영역 적용 by 후속 로직)
            static const json empty_array = json::array();
            const auto& all_roi_array = ( obj["Data"].contains("Roi") && obj["Data"]["Roi"].is_array() )
                ? obj["Data"]["Roi"]
                : empty_array;
            for( const auto& each_roi_obj : all_roi_array ) {
                // rois_array : 3차 Array : 1개 이상의 Roi array
                if( !each_roi_obj.is_object() || !each_roi_obj.contains("Points") || !each_roi_obj["Points"].is_array() ) {
                    MLOG_WARN("ScheduleSettingData: schedule_id=%d Roi 항목의 'Points' 누락/잘못된 타입. skip.", schedule_id);
                    continue;
                }
                const json& rois_array = each_roi_obj["Points"];

                // each_roi_obj 들의 Points를 임시 저장하는 값
                ROI<float> _rois;

                // each_roi_points : 2차 Array : 개별 Roi의 coords array
                for( const auto& each_roi_coords : rois_array ) {
                    if( !each_roi_coords.is_array() ) continue;
                    ROI_OneArea<float> _roi;
                    // coords : 1차 Array : Roi를 구성하는 개별 좌표값
                    for( const auto& coords : each_roi_coords ) {
                        if( !coords.is_array() || coords.size() < 2 ) continue;
                        // W-06: coord type 검증 추가
                        if( !coords[0].is_number() || !coords[1].is_number() ) continue;
                        _roi.push_back( ROI_OnePoint<float> { coords[0].get<float>(), coords[1].get<float>() } );
                    }
                    if( !_roi.empty() ) {
                        // 임시 총괄 관심 영역값에 추가
                        _rois.push_back( _roi );
                    }
                }

                if( !_rois.empty() ) {
                    // 임시 총괄 관심 영역을 TypeCode에 따라 스케줄 roi 중 하나에 insert
                    const int type_code = each_roi_obj.value( "TypeCode", Abnormal::RoiType::IncludeArea );
                    switch( type_code )
                    {
                    case Abnormal::RoiType::IncludeArea: sch.roi.insert( sch.roi.end(), _rois.begin(), _rois.end() ); break;
                    case Abnormal::RoiType::ExcludeArea: sch.exclude_roi.insert( sch.exclude_roi.end(), _rois.begin(), _rois.end() ); break;
                    case Abnormal::RoiType::TwoWayLine:  sch.two_way_roi.insert( sch.two_way_roi.end(), _rois.begin(), _rois.end() ); break;
                    case Abnormal::RoiType::OneWayLine:  sch.one_way_roi.insert( sch.one_way_roi.end(), _rois.begin(), _rois.end() ); break;
                    default: break;
                    }
                }
            }
            // 만약 포함영역( sch.roi )이 empty 라면 전체 영역으로 배치
            if( sch.roi.empty() ) {
                ROI_OneArea<float> _roi;
                _roi.push_back( ROI_OnePoint<float> { 0.0f, 0.0f } );
                _roi.push_back( ROI_OnePoint<float> { 0.0f, 1.0f } );
                _roi.push_back( ROI_OnePoint<float> { 1.0f, 1.0f } );
                _roi.push_back( ROI_OnePoint<float> { 1.0f, 0.0f } );
                sch.roi.push_back( _roi );
            }

            // BlackList
            // W-06: BlackList.Data 가 array 인지 + 각 element 가 string 인지 검증
            if( obj["Data"].contains("BlackList") && obj["Data"]["BlackList"].contains("Data") &&
                obj["Data"]["BlackList"]["Data"].is_array() ) {
                for( const auto& date : obj["Data"]["BlackList"]["Data"] ) {
                    if( date.is_string() )
                        sch.black_list_days.push_back( date.get<std::string>() );
                }
            }

            // Extra Settings
            if( obj["Data"]["Rule"].contains( "ExtraSettings" ) ) {
                auto& extra_settings = obj["Data"]["Rule"]["ExtraSettings"];
                sch.notification_min_interval_sec = extra_settings.value( "NotificationDelaySec", MINIMUM_EVENT_ACTIVE_INTERVAL_SEC );
                sch.loitering_require_dur_sec     = extra_settings.value( "LoiteringDurationSec", DEFAULT_EVENT_LOITER_DURATION_SEC );
            }
            else {
                sch.notification_min_interval_sec = MINIMUM_EVENT_ACTIVE_INTERVAL_SEC;
                sch.loitering_require_dur_sec     = DEFAULT_EVENT_LOITER_DURATION_SEC;
            }

            if( sch.notification_min_interval_sec < 1 )
                sch.notification_min_interval_sec = MINIMUM_EVENT_ACTIVE_INTERVAL_SEC;

            // ADD
            updateSchedules.push_back( std::move( sch ) );
        }
        // for( const auto& obj : json_array ) END
        if( skipped_count > 0 ) {
            MLOG_WARN("ScheduleSettingData: %zu/%zu schedules skipped due to validation. %zu accepted.",
                skipped_count, json_array.size(), updateSchedules.size() );
        }
        std::swap( setting->schedules, updateSchedules );

        return true;
    }

    void CameraSettingData::Show( void ) const
    {
        MLOG_INFO("  # CAM[%d] : %s", camera_id, url.empty() ? "N/A" : url.c_str());
    }

    // ServerSettingData
    ServerSettingData::ServerSettingData() noexcept
        : ISettingData( UpdateMode::FirstOnly )
    {
        this->SetUpdater( Impl_ServerSettingData_UpdateFromJsonObject );
    }

    ServerSettingData::ServerSettingData( const nlohmann::json& init_json_data ) noexcept
        : ISettingData( UpdateMode::FirstOnly )
    {
        this->SetUpdater( Impl_ServerSettingData_UpdateFromJsonObject );
        this->UpdateFromJson( init_json_data );
    }

    // CameraSettingData
    //   bugprone-exception-escape (audit 2026-05-19) — noexcept ctor 안 SetUpdater/UpdateFromJson 가
    //   throw 가능 → std::terminate. CLAUDE.md "외부 lib throw 흡수" 패턴으로 catch(...).
    //   NOLINT: base ctor (ISettingData) 의 member init list throw escape 는 base 구조상 불가피.
    //   (실제 throw 는 try/catch 로 흡수, FP).
    // NOLINTNEXTLINE(bugprone-exception-escape)
    CameraSettingData::CameraSettingData() noexcept
        : ISettingData( UpdateMode::FirstOnly )
    {
        try {
            this->SetUpdater( Impl_CameraSettingData_UpdateFromJsonObject );
        } catch( ... ) {
            // noexcept ctor 안 throw 흡수. ctor 실패 시 객체는 default state (Updater 미설정).
        }
    }

    // NOLINTNEXTLINE(bugprone-exception-escape)
    CameraSettingData::CameraSettingData( const nlohmann::json& init_json_data ) noexcept
        : ISettingData( UpdateMode::FirstOnly )
    {
        try {
            this->SetUpdater( Impl_CameraSettingData_UpdateFromJsonObject );
            this->UpdateFromJson( init_json_data );
        } catch( ... ) {
            // noexcept ctor 안 throw 흡수. JSON parse 실패 시 객체는 partial state.
        }
    }

    // ExcludeCamSettingData
    ExcludeCamSettingData::ExcludeCamSettingData() noexcept
        : ISettingData( UpdateMode::FirstOnly )
    {
        this->SetUpdater( Impl_ExcludeCamSettingData_UpdateFromJsonObject );
    }

    ExcludeCamSettingData::ExcludeCamSettingData( const nlohmann::json& init_json_data ) noexcept
        : ISettingData( UpdateMode::FirstOnly )
    {
        this->SetUpdater( Impl_ExcludeCamSettingData_UpdateFromJsonObject );
        this->UpdateFromJson( init_json_data );
    }

    // ScheduleSettingData
    // BUG FIX: ctor 의 UpdateMode 가 FirstOnly 였으나, ScheduleSettingData 는
    // 카메라당 N개의 schedule 을 array 전체로 처리해야 함 (Impl_*_UpdateFromJsonArray).
    // FirstOnly 면 array 의 첫 element 만 updater 에 전달되어 is_array()=false → 항상 실패.
    // 운영 중 모든 카메라의 schedule 이 적용 안 되던 root cause (graceful degradation 으로 드러남).
    ScheduleSettingData::ScheduleSettingData() noexcept
        : ISettingData( UpdateMode::FullArray )
    {
        this->SetUpdater( Impl_ScheduleSettingData_UpdateFromJsonArray );
    }

    ScheduleSettingData::ScheduleSettingData( const nlohmann::json& init_json_data ) noexcept
        : ISettingData( UpdateMode::FullArray )
    {
        this->SetUpdater( Impl_ScheduleSettingData_UpdateFromJsonArray );
        this->UpdateFromJson( init_json_data );
    }

} // namespace MGEN
