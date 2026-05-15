#pragma once

// BasicLibs
#include "MgenTypes.h"
#include "json/json.hpp"
#include "AbnormalEventTypes.h"

// SettingMAnager Interface
#include "ISettingData.h"

// STL
#include <set>
#include <vector>
#include <utility>

namespace MGEN
{
    namespace DefineDefault
    {
        DEFINE DFPS_LIMIT_PER_UNIT             = 30;
        DEFINE TRACKING_OBJECT_HIST_LIMIT      = 200;
        DEFINE RTSP_PROXY_SYNC_DELAY_MS        = 100;
        DEFINE RTSP_PROXY_PORT                 = 555;
        DEFINE FULL_FRAME_IMAGE_SAVE_ROOT_PATH = "/frame";
        DEFINE CROP_FRAME_IMAGE_SAVE_ROOT_PATH = "/crop";

        DEFINE MIN_ABOUT_1_DAY                   = 1440;
        DEFINE DAY_ABOUT_1_WEEK                  = 7;
        DEFINE WEEKLY_DAY_ON                     = 1;

        DEFINE MINIMUM_EVENT_ACTIVE_INTERVAL_SEC = 60;	// - minimum interval for event notify at same schedule
        DEFINE DEFAULT_EVENT_REQUIR_DURATION_SEC = 1;	// - require duration at same history
        DEFINE DEFAULT_EVENT_LOITER_DURATION_SEC = 60;	// - require duration at same history (VehicleParking)
        DEFINE DEFAULT_EVENT_FRAME_RELEASE_S_SEC = 10;	// - inactive history release sec
        DEFINE DEFAULT_EVENT_FRAME_RELEASE_L_SEC = 120;	// - inactive history release sec (VehicleParking)
    }

    template <typename T> using ROI_OnePoint = std::pair<T, T>;
    template <typename T> using ROI_OneArea  = std::vector<ROI_OnePoint<T>>;
    template <typename T> using ROI_OneLine  = std::pair<ROI_OnePoint<T>, ROI_OnePoint<T>>;
    template <typename T> using ROI          = std::vector<ROI_OneArea<T>>;

    class ServerSettingData final : public ISettingData
    {
    public:
        // constructor
        ServerSettingData() noexcept;
        ServerSettingData( const nlohmann::json& init_json_data ) noexcept;

        // destructor
        ~ServerSettingData() = default;

    // From MVAS
    // P39: 멤버 default 값은 DefineDefault namespace 의 단일 진실로 통일 (DRY).
    public:
        int            service_server_id            = 0;
        int            inference_per_cams_fps_limit = DefineDefault::DFPS_LIMIT_PER_UNIT;
        int            tracking_memory_object_limit = DefineDefault::TRACKING_OBJECT_HIST_LIMIT;
        int            rtsp_proxies_sync_delay_ms   = DefineDefault::RTSP_PROXY_SYNC_DELAY_MS;
        int            rtsp_proxy_publish_port      = DefineDefault::RTSP_PROXY_PORT;
        // 서비스 내 이미지 저장 경로 (컨테이너 내부). MVAS 응답에서 채워지지 않으면 DefineDefault 의 기본값 사용.
        std::string    full_image_save_root_path    = DefineDefault::FULL_FRAME_IMAGE_SAVE_ROOT_PATH;
        std::string    crop_image_save_root_path    = DefineDefault::CROP_FRAME_IMAGE_SAVE_ROOT_PATH;
    };

    class CameraSettingData : public ISettingData
    {
    public:
        // constructor
        CameraSettingData() noexcept;
        CameraSettingData( const nlohmann::json& init_json_data ) noexcept;

        // destructor
        ~CameraSettingData() = default;

        // Util : Show
        void Show( void ) const;

    public:
        int            camera_id   = 00;
        std::string    url         = "";
        std::string    access_ip   = "";
        std::string    access_id   = "";
        std::string    access_pw   = "";
        nlohmann::json extend_data;
    };

    class ExcludeCamSettingData : public ISettingData
    {
    public:
        // constructor
        ExcludeCamSettingData() noexcept;
        ExcludeCamSettingData( const nlohmann::json& init_json_data ) noexcept;

        // destructor
        ~ExcludeCamSettingData() = default;

    public:
        bool is_exclude = false;
    };

    namespace Abnormal
    {
        enum EventLevel {
            LowLevel    = 1,
            MiddleLevel = 2,
            HighLevel   = 3,
            TopLevel    = 4,
        };

        enum RoiType {
            IncludeArea = 1, // 포함 영역
            ExcludeArea = 2, // 예외 영역
            TwoWayLine  = 3, // 양방향 선
            OneWayLine  = 4, // 단방향 선 (clockwise)
        };
    }

    class ScheduleSettingData : public ISettingData
    {
    public:
        struct AbnormalEventScheduleInfo {
            int schedule_id;                                                     // UID per each schedule
            Abnormal::EventClass event_code = MGEN::Abnormal::EventClass::None;  // enum EventClass
            int level_code = Abnormal::EventLevel::LowLevel;                     // enum EventLevel

            // About Time range
            std::string      start_time;    // "23:00"
            int              range_minutes; // 만약 5시간이라면 300, 전체 시간 검출이라면 1440(60x24)
            std::vector<int> weekly;        // 일요일부터 시작해서, [ 1, 1, 1, 0, 1, 0, 0 ] 처럼 정의. 사용 요일 1, 미사용 요일 0

            /** ROI( Region of Interest ) :
             * 관심 영역이 한 스케줄 내 여러 구역일 수 있으므로 'roi 점 좌표 벡터' 들의 벡터
             * 점 좌표들은 절대좌표가 아닌 상대좌표 ( 0.0 ~ 1.0 )
             */
            ROI<float> roi;         // IncludeArea
            ROI<float> exclude_roi; // ExcludeArea
            ROI<float> two_way_roi; // TwoWayLine
            ROI<float> one_way_roi; // OneWayLine

            // BlackList
            std::vector<std::string> black_list_days; // "yyyy-MM-dd" list

            // Extra setting data
            int   notification_min_interval_sec; // 동일 스케줄에서 이벤트 발생 Notify 최소 텀
            int   loitering_require_dur_sec;     // VehicleParking 이벤트 발생 최소 요구 시간초
        };

    public:
        // Constructor
        ScheduleSettingData() noexcept;
        ScheduleSettingData( const nlohmann::json& init_json_data ) noexcept;

        // Destructor
        ~ScheduleSettingData() = default;

    public:
        std::vector<AbnormalEventScheduleInfo> schedules;
    };


} // namespace MGEN
