#include "RtspDetectorUnit.h"

// Common Modules - BasicLibs
#include "MgenLogger.h"        // 로그 출력을 위해 포함
#include "CorrelationContext.h" // P53: thread-local correlation_id (시스템 thread 진입점)
#include "MetricsRegistry.h"    // P54: errors_total / events_total counter
#include "file_utils.h"        // 디렉토리 생성 및 파일 체크를 위해 포함
#include "EngineStreamTypes.h" // EngineStreamMetaData, ResolutionKey 등 참조를 위해 포함
#include "MgenTypes.h"         // UnitID, CameraColorMode 등 기초 타입 참조
#include "InferObject.h"       // InferObject 및 ImageExpressStyle 사용을 위해 포함
#include "ClassChecker.h"      // 엔진 프로파일의 클래스 검증을 위해 포함

// Common Modules - VisionCommon
#include "FramePreProcessor.h"      // 영상 전처리(Resize/Padding) 기능을 위해 포함
#include "SwsContextManager.h"      // FFmpeg SwsContext 캐싱 관리를 위해 포함

// Common Modules - Management
#include "EngineClient.h"     // 엔진 클라이언트 관리 객체 참조를 위해 포함
#include "SettingData.h"       // 서버 및 카메라 설정 데이터 참조를 위해 포함

// External Libraries
#include "json/json.hpp"       // JSON 파싱 및 생성을 위해 포함
#include <chrono>              // 시간 측정 및 대기를 위해 포함
#include <future>              // 비동기 처리를 위해 포함
#include <vector>              // 컨테이너 사용을 위해 포함
#include <algorithm>           // 정렬 및 데이터 처리를 위해 포함
#include <cmath>               // 산술 연산 및 반올림을 위해 포함
#include <cassert>             // 런타임 로직 검증을 위해 포함
#include <functional>

// FFmpeg 라이브러리 (필요한 최소 타입 참조)
extern "C" {
#include <libavutil/frame.h>   // AVFrame 구조체 참조를 위해 포함
#include <libavutil/pixfmt.h>  // AVPixelFormat 정의 참조를 위해 포함
}

#include <sys/statvfs.h>       // P54 Layer 1: /frame disk usage 체크용 (statvfs)
#include <filesystem>          // P54 Layer 2: 자동 청소 — 7일 이전 디렉토리 삭제용
#include <malloc.h>            // glibc malloc_trim: emergency cleanup 후 heap 강제 반환 (W-14)

namespace MGEN
{
    using namespace std::chrono_literals;
    using nlohmann::json;

    inline static bool IsOverTime( const MGEN::EventTime& t1, const MGEN::EventTime& t2, std::chrono::milliseconds lapse )
    {
        const auto diff = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t2).count();
        return std::abs(diff) > lapse.count();
    }

    // ============================================================================
    // P54 — /frame 디스크 방어 정책 (3 Layer)
    //   Layer 1: imwrite 직전 사전 차단 (>= 90% 사용 시 skip)
    //   Layer 2: 7일 이전 일자 폴더 자동 삭제 (cleanup)
    //   Layer 3: 디스크 사용량 + skip + cleanup 메트릭
    // ============================================================================
    namespace
    {
        // P61: 절대경로는 DefineDefault namespace 의 단일 진실 사용 (DRY).
        constexpr auto         FRAME_DISK_PATH           = DefineDefault::FULL_FRAME_IMAGE_SAVE_ROOT_PATH;
        constexpr double       FRAME_DISK_FULL_PCT       = 90.0;   // L1 사전 차단 임계
        constexpr double       FRAME_DISK_EMERGENCY_PCT  = 80.0;   // L2-Emergency 비상 청소 임계
        constexpr int          FRAME_RETENTION_DAYS      = 7;      // L2-Regular 보존 기간
        constexpr auto         FRAME_CLEANUP_INTERVAL    = std::chrono::hours( 1 );    // 정기 청소 주기
        constexpr auto         FRAME_EMERGENCY_INTERVAL  = std::chrono::minutes( 5 );  // 비상 청소 cool-down

        // /frame 디스크 사용량 [0, 100]. 실패 시 -1.
        double GetFrameDiskUsedPercent() noexcept
        {
            struct statvfs s {};
            if( ::statvfs( FRAME_DISK_PATH, &s ) != 0 ) return -1.0;
            if( s.f_blocks == 0 ) return -1.0;
            return 100.0 *
                static_cast<double>( s.f_blocks - s.f_bavail ) /
                static_cast<double>( s.f_blocks );
        }

        // /frame 디스크 used / capacity bytes. 실패 시 둘 다 0.
        struct FrameDiskBytes { double used; double capacity; };
        FrameDiskBytes GetFrameDiskBytes() noexcept
        {
            struct statvfs s {};
            if( ::statvfs( FRAME_DISK_PATH, &s ) != 0 ) return { 0.0, 0.0 };
            const double frsize = static_cast<double>( s.f_frsize );
            const double cap    = static_cast<double>( s.f_blocks ) * frsize;
            const double avail  = static_cast<double>( s.f_bavail ) * frsize;
            return { cap - avail, cap };
        }

        // /frame/<YYYY>/<MM>/<DD>/ 형태의 일자 폴더 중 cutoff 보다 오래된 것 삭제.
        // 빈 월/년 폴더도 함께 정리. 삭제된 일자 폴더 수 반환.
        // statvfs / filesystem 에러는 silently 무시 (다음 주기에 재시도).
        size_t CleanupOldFrameDirs( const int retention_days ) noexcept
        {
            namespace fs = std::filesystem;

            const auto now    = std::chrono::system_clock::now();
            const auto cutoff = now - std::chrono::hours( 24 * retention_days );

            std::error_code ec;
            const fs::path frame_root { FRAME_DISK_PATH };
            if( !fs::exists( frame_root, ec ) ) return 0;

            size_t deleted = 0;

            // 디렉토리 이름 (YYYY/MM/DD) 으로 시각 계산. last_write_time 보다 안정적.
            auto parse_dir_time = []( const std::string& y, const std::string& m, const std::string& d )
                -> std::optional<std::chrono::system_clock::time_point>
            {
                try {
                    std::tm t {};
                    t.tm_year = std::stoi( y ) - 1900;
                    t.tm_mon  = std::stoi( m ) - 1;
                    t.tm_mday = std::stoi( d );
                    t.tm_hour = 23; t.tm_min = 59; t.tm_sec = 59; // 일자 끝 기준
                    const time_t tt = ::timegm( &t );
                    if( tt == static_cast<time_t>( -1 ) ) return std::nullopt;
                    return std::chrono::system_clock::from_time_t( tt );
                }
                catch( ... ) { return std::nullopt; }
            };

            for( const auto& yr : fs::directory_iterator( frame_root, ec ) ) {
                if( !yr.is_directory( ec ) ) continue;
                for( const auto& mo : fs::directory_iterator( yr.path(), ec ) ) {
                    if( !mo.is_directory( ec ) ) continue;
                    for( const auto& dy : fs::directory_iterator( mo.path(), ec ) ) {
                        if( !dy.is_directory( ec ) ) continue;
                        const auto t_opt = parse_dir_time(
                            yr.path().filename().string(),
                            mo.path().filename().string(),
                            dy.path().filename().string() );
                        if( !t_opt.has_value() ) continue;
                        if( *t_opt < cutoff ) {
                            const auto removed = fs::remove_all( dy.path(), ec );
                            if( !ec && removed > 0 ) ++deleted;
                        }
                    }
                    // 빈 월 폴더 정리
                    if( fs::is_empty( mo.path(), ec ) ) fs::remove( mo.path(), ec );
                }
                // 빈 년 폴더 정리
                if( fs::is_empty( yr.path(), ec ) ) fs::remove( yr.path(), ec );
            }

            return deleted;
        }

        // L2-Emergency: 디스크 사용률 >= 80% 시 과거 날짜 폴더부터 1개씩 삭제.
        // 폴더가 1개만 남고 그게 당일이면 그 안 파일의 오래된 절반 삭제.
        // 반환: { 삭제된 일자 폴더 수, 삭제된 파일 수 }.
        struct EmergencyCleanupResult { size_t deleted_day_dirs; size_t deleted_files; };

        // IF-02: 다중 IOWorker (카메라 N대) 의 emergency cleanup race 방지.
        // try_to_lock 으로 동시 1개만 진행 — 다른 worker 는 다음 주기에 재시도.
        static std::mutex g_emergency_cleanup_mtx;

        EmergencyCleanupResult EmergencyCleanupIfDiskHigh() noexcept
        {
            EmergencyCleanupResult res { 0, 0 };

            // 다른 worker 가 진행 중이면 skip — 다음 주기에 재시도
            std::unique_lock<std::mutex> lock( g_emergency_cleanup_mtx, std::try_to_lock );
            if( !lock.owns_lock() ) return res;

            double pct = GetFrameDiskUsedPercent();
            if( pct < FRAME_DISK_EMERGENCY_PCT ) return res;

            namespace fs = std::filesystem;
            std::error_code ec;
            const fs::path frame_root { FRAME_DISK_PATH };
            if( !fs::exists( frame_root, ec ) ) return res;

            // /frame/<YYYY>/<MM>/<DD>/ 모든 일자 폴더 수집 (path 정렬 = 시간 정렬).
            std::vector<fs::path> day_dirs;
            for( const auto& yr : fs::directory_iterator( frame_root, ec ) ) {
                if( !yr.is_directory( ec ) ) continue;
                for( const auto& mo : fs::directory_iterator( yr.path(), ec ) ) {
                    if( !mo.is_directory( ec ) ) continue;
                    for( const auto& dy : fs::directory_iterator( mo.path(), ec ) ) {
                        if( dy.is_directory( ec ) ) day_dirs.push_back( dy.path() );
                    }
                }
            }
            std::sort( day_dirs.begin(), day_dirs.end() );

            // case 1: 폴더 2개 이상 — 가장 오래된 폴더부터 삭제 + 사용률 재확인 반복.
            while( day_dirs.size() > 1 && pct >= FRAME_DISK_EMERGENCY_PCT ) {
                const auto& oldest = day_dirs.front();
                fs::remove_all( oldest, ec );
                ++res.deleted_day_dirs;
                day_dirs.erase( day_dirs.begin() );
                pct = GetFrameDiskUsedPercent();
            }

            // case 2: 폴더 1개만 남고 (당일) 여전히 high — 그 안 파일의 오래된 절반 삭제.
            if( day_dirs.size() == 1 && pct >= FRAME_DISK_EMERGENCY_PCT ) {
                std::vector<fs::directory_entry> files;
                for( const auto& f : fs::directory_iterator( day_dirs.front(), ec ) ) {
                    if( f.is_regular_file( ec ) ) files.push_back( f );
                }
                // mtime 오름차순 (오래된 것이 앞).
                std::sort( files.begin(), files.end(),
                    []( const fs::directory_entry& a, const fs::directory_entry& b ){
                        std::error_code ec_local;
                        return fs::last_write_time( a, ec_local ) < fs::last_write_time( b, ec_local );
                    });
                const size_t half = files.size() / 2;
                for( size_t i = 0; i < half; ++i ) {
                    fs::remove( files[i], ec );
                    if( !ec ) ++res.deleted_files;
                }
            }

            // 빈 월/년 폴더 정리.
            for( const auto& yr : fs::directory_iterator( frame_root, ec ) ) {
                if( !yr.is_directory( ec ) ) continue;
                for( const auto& mo : fs::directory_iterator( yr.path(), ec ) ) {
                    if( mo.is_directory( ec ) && fs::is_empty( mo.path(), ec ) ) {
                        fs::remove( mo.path(), ec );
                    }
                }
                if( fs::is_empty( yr.path(), ec ) ) fs::remove( yr.path(), ec );
            }

            // W-14: glibc malloc 의 heap arena 를 OS 로 강제 반환.
            //   cleanup 안의 std::filesystem path string 할당/해제 burst 로 인한
            //   fragmentation 누적 회피. 48h 테스트 시 후반 10h 에서 RssAnon
            //   +47 MB 증가 관측 → malloc_trim 으로 해소 시도.
            malloc_trim( 0 );

            return res;
        }
    } // namespace

    // [DEBUG VIRTUAL LINES — REMOVABLE BLOCK START]
    // Phase 2 (IO Worker) 효과 검증 / 시연 / 이벤트 빈발 시뮬레이션 목적.
    // 모든 카메라에 가상 schedule 2개 추가:
    //   - ID 99999: LineIntrusion (사람) — 가로 3 + 세로 3 = 6개 라인
    //   - ID 99998: VehicleIntrusion (차량) — 동일 6개 라인
    // 지울 때:
    //   grep -n "DEBUG VIRTUAL LINES" 로 4곳 (block 시작/끝 + 호출 2곳) 식별 후 모두 제거.
    //   상세 안내: README.md 의 "Debug 하드코딩 제거" 섹션
    static void AddDebugVirtualLines_REMOVABLE( ScheduleSettingData& data ) noexcept
    {
        // 6개 라인 (가로 3 + 세로 3) — LineIntrusion / VehicleIntrusion 공통 사용
        const auto build_two_way_lines = []() {
            ROI<float> lines;
            // 가로 3개 (y = 0.25, 0.5, 0.75; x: 0~1)
            lines.push_back( { { 0.0f, 0.25f }, { 1.0f, 0.25f } } );
            lines.push_back( { { 0.0f, 0.50f }, { 1.0f, 0.50f } } );
            lines.push_back( { { 0.0f, 0.75f }, { 1.0f, 0.75f } } );
            // 세로 3개 (x = 0.25, 0.5, 0.75; y: 0~1)
            lines.push_back( { { 0.25f, 0.0f }, { 0.25f, 1.0f } } );
            lines.push_back( { { 0.50f, 0.0f }, { 0.50f, 1.0f } } );
            lines.push_back( { { 0.75f, 0.0f }, { 0.75f, 1.0f } } );
            return lines;
        };

        // Schedule #1: LineIntrusion (사람)
        ScheduleSettingData::AbnormalEventScheduleInfo person_sch {};
        person_sch.schedule_id                    = 99999;
        person_sch.event_code                     = Abnormal::EventClass::LineIntrusion;
        person_sch.level_code                     = Abnormal::EventLevel::LowLevel;
        person_sch.start_time                     = "00:00";
        person_sch.range_minutes                  = 24 * 60;             // 24시간 전체
        person_sch.weekly                         = { 1, 1, 1, 1, 1, 1, 1 }; // 모든 요일
        person_sch.two_way_roi                    = build_two_way_lines();
        person_sch.notification_min_interval_sec  = 1;                    // 짧게 (빈발 유도)
        person_sch.loitering_require_dur_sec      = 0;                    // LineIntrusion 무관
        data.schedules.push_back( std::move( person_sch ) );

        // Schedule #2: VehicleIntrusion (차량)
        ScheduleSettingData::AbnormalEventScheduleInfo vehicle_sch {};
        vehicle_sch.schedule_id                   = 99998;
        vehicle_sch.event_code                    = Abnormal::EventClass::VehicleIntrusion;
        vehicle_sch.level_code                    = Abnormal::EventLevel::LowLevel;
        vehicle_sch.start_time                    = "00:00";
        vehicle_sch.range_minutes                 = 24 * 60;
        vehicle_sch.weekly                        = { 1, 1, 1, 1, 1, 1, 1 };
        vehicle_sch.two_way_roi                   = build_two_way_lines();
        vehicle_sch.notification_min_interval_sec = 1;
        vehicle_sch.loitering_require_dur_sec     = 0;                    // VehicleParking 만 의미 있음
        data.schedules.push_back( std::move( vehicle_sch ) );
    }
    // [DEBUG VIRTUAL LINES — REMOVABLE BLOCK END]

    RtspDetectorUnit::RtspDetectorUnit
    (
        const MGEN::Type::UnitID            service_unit_id,
        const ServiceBlockProfile&          /*service_block_profile*/,
        std::shared_ptr<NetworkManager>     network_manager,
        std::shared_ptr<IOStreamManager>    io_stream_manager,
        std::shared_ptr<EngineLoadBalancer> load_balancer
    )
        : id_              ( service_unit_id   )
        , load_balancer_   ( load_balancer     )
        , network_manager_ ( network_manager   )
        , iostream_manager_( io_stream_manager )
    {
        //
    }

    RtspDetectorUnit::~RtspDetectorUnit()
    {
        // 멤버 destroy 이전에 setting callback registry 명시 정리.
        // raw [this] 캡처 callback 이 SocketIO message thread 에서 dangling 접근하는 UAF 차단.
        this->ClearAllSubscriptions();
        this->Stop();
    }

    // Init & Check
    bool RtspDetectorUnit::Init( void )
    {
        if( this->id_ == UNIT_ID_NOT_SET ) {
            MLOG_ERROR("CAM[%d]::Init(), 'id_' is not set.", id_);
            return false;
        }

        if( !this->load_balancer_ ) {
            MLOG_ERROR("CAM[%d]::Init(), 'load_balancer_' is invalid.", id_);
            return false;
        }

        // ----------------------------------------------------------------------------------------------

        if( !this->network_manager_ ){
            MLOG_ERROR("CAM[%d]::Init(), 'network_manager_' is invalid.", id_);
            return false;
        }

        this->rtsp_handler_ = this->network_manager_->GetRtspHandler();
        if( !this->rtsp_handler_ ){
            MLOG_ERROR("CAM[%d]::Init(), 'rtsp_handler_' is invalid.", id_);
            return false;
        }

        this->sio_handler_ = this->network_manager_->GetSioHandler();
        if( !this->sio_handler_ ){
            MLOG_ERROR("CAM[%d]::Init(),'sio_handler_' is invalid.", id_);
            return false;
        }

        this->socket_io_server_id_ = sio_handler_->GetID();
        if( this->socket_io_server_id_ == DefineDefault::SERVER_IDENTIFIER_NOT_SET ){
            MLOG_ERROR("CAM[%d]::Init(), 'socket_io_server_id_' is not set.", id_);
            return false;
        }

        // ----------------------------------------------------------------------------------------------

        if( !this->iostream_manager_ ){
            MLOG_ERROR("CAM[%d]::Init(), 'iostream_manager_' is invalid.", id_);
            return false;
        }

        // Queue : IN - avframe
        this->avframe_q_ = this->iostream_manager_->GetSafeQueue<std::shared_ptr<AVFrame>>( QueueType::RTSP_AVFRAME, id_ );
        if( !this->avframe_q_ ){
            MLOG_ERROR("CAM[%d]::Init(), 'avframe_q_' is created failed.", id_);
            return false;
        }
        // F-F3-01: capacity 명시 (RTSP burst 시 누적 메모리 보호).
        // 2 * detect_fps_limit_ = 1초치 frame buffer. 초과 시 oldest drop.
        this->avframe_q_->SetMaxSize( 2 * static_cast<size_t>( this->detect_fps_limit_ ) );

        // ----------------------------------------------------------------------------------------------

        // Set Thread Runner/Closer ======================================================================
        this->inference_thread_.SetThreadFunctions(
            std::bind( &RtspDetectorUnit::InferenceThreadRunner, this ),
            std::bind( &RtspDetectorUnit::InferenceThreadCloser, this )
        );

        // IO Worker (Stage G cv::imwrite 비동기 처리)
        // 이벤트 빈발 시 main loop 의 frame drop 차단 + tracker 정확도 유지.
        // Capacity 30 (cv::Mat ~6MB × 30 = ~180MB). 한계 초과 시 oldest drop (P40 패턴).
        this->io_work_queue_ = std::make_unique<SafeQueue<IOWorkItem>>();
        this->io_work_queue_->SetMaxSize( 30 );
        this->io_worker_thread_.SetThreadFunctions(
            std::bind( &RtspDetectorUnit::IOWorkerThreadRunner, this ),
            std::bind( &RtspDetectorUnit::IOWorkerThreadCloser, this )
        );

        // Setting Manager Get Check =====================================================================
        auto sm = MGEN::GetSettingManager();
        if( !sm ){
            MLOG_ERROR("CAM[%d]::Init(), 'SettingManager' is load failed.", id_);
            return false;
        }

        // Set from SettingManager ========================================================================
        // Init setting data first value : exclude cam setting
        if( auto exclude_data_opt = sm->GetExcludeCamSetting( id_ ); exclude_data_opt.has_value() ) {
            std::lock_guard<std::mutex> exc_lck { this->exclude_setting_mtx_ };
            this->exclude_setting_ = *exclude_data_opt;
        }
        // 예외 영역 없을 수도 있음 (정상 시나리오)

        // Init setting data first value : schedule setting
        // [DEBUG VIRTUAL LINES] schedule 없는 카메라에도 가상 라인 강제 적용 → lock 항상 잡고 처리
        {
            std::lock_guard<std::mutex> sch_lck { this->schedule_settings_mtx };
            if( auto schedule_data_opt = sm->GetScheduleSetting( id_ ); schedule_data_opt.has_value() ) {
                this->schedule_settings_ = *schedule_data_opt;
            }
            AddDebugVirtualLines_REMOVABLE( this->schedule_settings_ ); // [DEBUG VIRTUAL LINES — REMOVE]
            if( this->schedule_settings_.schedules.size() > 0 ){
                this->is_schedule_updated_.store( true );
            }
        }
        // 스케줄 없을 수도 있음 (정상 시나리오)

        // Set from SettingManager ========================================================================
        // subscribe schedule setting callback
        SubscribeSetting<ExcludeCamSettingData>(
            [this](const ExcludeCamSettingData& newData)
            {
                std::lock_guard<std::mutex> lck { this->exclude_setting_mtx_ };
                this->exclude_setting_ = newData;
            },
            id_,
            sm
        );

        SubscribeSetting<ScheduleSettingData>(
            [this](const ScheduleSettingData& newData)
            {
                std::lock_guard<std::mutex> lck { this->schedule_settings_mtx };
                this->schedule_settings_ = newData;
                AddDebugVirtualLines_REMOVABLE( this->schedule_settings_ ); // [DEBUG VIRTUAL LINES — REMOVE]
                this->is_schedule_updated_.store( true );
            },
            id_,
            sm
        );

        // get setting value from setting manager
        if( auto cs_opt = sm->GetServerSetting(); cs_opt.has_value() )
        {
            this->detect_fps_limit_ = cs_opt->inference_per_cams_fps_limit;
        }

        // 초기 실시간 FPS를 최대 30fps로 설정
        this->realtime_fps_ = std::min( this->detect_fps_limit_, 30 );

        this->is_initialized_service_ = true;
        return true;
    }

    bool RtspDetectorUnit::Start( void )
    {
        if( this->is_initialized_service_ == false ){
            MLOG_WARN("DETECTOR::Init() not yet, run Init() before Start().");
            if( this->Init() == false ){
                return false;
            }
        }

        // IO worker 먼저 start — main loop 가 첫 frame 부터 enqueue 가능하도록.
        if( this->io_worker_thread_.Start() == false )
            return false;

        if( this->inference_thread_.Start() == false )
            return false;

        return true;
    }

    bool RtspDetectorUnit::Stop( void )
    {
        // 순서: inference (producer) 먼저 stop → io worker (consumer) 그 다음.
        // inference stop 후엔 enqueue 안 들어옴 → io_work_queue terminate 시 누락 없음.
        this->inference_thread_.Stop();
        this->io_worker_thread_.Stop();
        return true;
    }

    static const size_t ConvertTrackingBoxesToMetaData( const vector<InferObject>& infer_objects, string& metadata_string )
    {
        // get time
        auto        now   = std::chrono::system_clock::now();
        auto        ms    = std::chrono::duration_cast<std::chrono::milliseconds>( now.time_since_epoch() ) % 1000;
        std::time_t now_c = std::chrono::system_clock::to_time_t( now );

        struct tm tstruct;
        localtime_r(&now_c, &tstruct); // Thread-Safe한 localtime_r 사용

        char time_buf[80];
        // 포맷: 2025-12-04T15:30:00 (OS 시간대 기준)
        std::strftime(time_buf, sizeof(time_buf), "%Y-%m-%dT%H:%M:%S", &tstruct);

        std::stringstream ss_time;
        ss_time << time_buf << "." << std::setfill('0') << std::setw(3) << ms.count();
        std::string current_utc_time = ss_time.str(); // 변수명은 UtcTime이지만 실제 값은 LocalTime

        // XML 조합
        // preXml을 const static에서 일반 string 생성을 위해 분리하거나, sprintf 등으로 조합
        std::string xml_header =
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
            "<tt:MetadataStream xmlns:tt=\"http://www.onvif.org/ver10/schema\">"
            "<tt:VideoAnalytics>"
            "<tt:Frame UtcTime=\"" + current_utc_time + "\">"; // [Modified] 동적 시간 삽입

        const static std::string postXml =
            "</tt:Frame>"
            "</tt:VideoAnalytics>"
            "</tt:MetadataStream>\0\0";

        metadata_string.reserve( 2048 );
        metadata_string = "";
        metadata_string += xml_header;

        std::string objectxml = "";
        objectxml.reserve( 512 );

        for( const auto& infer_object : infer_objects ) {

            std::stringstream fProb;
            fProb.precision( 2 );
            fProb << std::fixed << infer_object.score;

            objectxml
                = "<tt:Object ObjectId=\"" + std::to_string( infer_object.class_id ) + "\">"
                    + "<tt:Appearance>"
                        + "<tt:Shape>"
                            + "<tt:BoundingBox left=\"" + std::to_string( static_cast<int>( infer_object.bbox.x ) ) + "\" "
                            + "top=\""                  + std::to_string( static_cast<int>( infer_object.bbox.y ) ) + "\" "
                            + "right=\""                + std::to_string( static_cast<int>( infer_object.bbox.w + infer_object.bbox.x ) ) + "\" "
                            + "bottom=\""               + std::to_string( static_cast<int>( infer_object.bbox.h + infer_object.bbox.y ) ) + "\" />"
                            + "<tt:TrackId trackid=\""  + std::to_string( static_cast<int>( infer_object.track_id ) ) + +"\" />"
                            + "<tt:CenterOfGravity x=\"60.0\" y=\"50.0\" />"
                        + "</tt:Shape>"
                        + "<tt:Class>"
                            + "<tt:ClassCandidate>"
                            + "<tt:Type>MaybeUnused</tt:Type>"
                            + "<tt:Likelihood>" + std::move( fProb.str() ) + "</tt:Likelihood>"
                            + "</tt:ClassCandidate>"
                        + "</tt:Class>"
                    + "</tt:Appearance>"
                + "</tt:Object>";
            metadata_string += std::move( objectxml );
        }
        metadata_string += postXml;

        return metadata_string.size() + 1;
    }

    void RtspDetectorUnit::SendDetectResultToMetaData( const vector<InferObject>& detect_results )
    {
        string meta_data = "";
        size_t meta_size = ConvertTrackingBoxesToMetaData( detect_results, meta_data );
        proxy_ptr_->runCallBacks( reinterpret_cast<uint8*>( const_cast<char*>( meta_data.c_str() ) ),
                                  static_cast<int>( meta_size ),
                                  DATA_TYPE_METADATA );
    }

    std::string RtspDetectorUnit::GetFrameImageCurrentProxyRootPath( void ) const
    {
        // P61: DefineDefault 의 단일 진실 사용.
        return std::string { DefineDefault::FULL_FRAME_IMAGE_SAVE_ROOT_PATH };
    }

    std::optional<std::string> RtspDetectorUnit::MakeImageSavePath( const std::string& root_path ) const
    {
        struct tm tstruct;
        auto  curr_time = std::time( nullptr );
        auto* time_info = localtime_r( &curr_time, &tstruct );

        const int n_year  = time_info->tm_year + 1900;
        const int n_month = time_info->tm_mon + 1;
        const int n_day   = time_info->tm_mday;

        char sz_dir_path[256] = { 0x00, };
        snprintf( sz_dir_path, sizeof(sz_dir_path), "%s/%04d/%02d/%02d", root_path.c_str(), n_year, n_month, n_day );

        if( MakeDirectoryWhenNotExist( sz_dir_path ) == false ){
            return std::nullopt;
        }

        const int n_hour  = time_info->tm_hour;
        const int n_min   = time_info->tm_min;
        const int n_sec   = time_info->tm_sec;

        char sz_file_path[128] = { 0x00, };
        snprintf( sz_file_path, 128, "/%04d_%02d%02d%02d_%s.jpg",
            id_, n_hour, n_min, n_sec,
            UUID::GetGenerator()->generate().c_str() );

        const std::string dir_path( sz_dir_path );
        const std::string img_path = dir_path + string( sz_file_path );

        return img_path;
    }

    json RtspDetectorUnit::BuildNotifyJsonBase
    (
        const std::string&               message_type,
        const SocketIO::TransmissionType trans_type,
        const tm*                        time_info,
        const std::string&               destination
    )
    {
        json res;

        // formmating time info
        char szTime[20] = { '\0', };
        std::strftime( szTime, sizeof( szTime ), "%Y-%m-%d %H:%M:%S", time_info );

        // Depth : 1
        json data;
        data["MessageType"]       = message_type;
        data["Message"]           = json();

        // Depth : 0
        res["TransmissionMethod"] = static_cast<int>( trans_type );
        res["Time"]               = szTime;
        res["Source"]             = to_string( this->socket_io_server_id_ );
        res["Destination"]        = destination;
        res["Data"]               = data;

        return res;
    }

    json RtspDetectorUnit::BuildNotifyJsonImpl_Analysis
    (
        std::shared_ptr<Abnormal::Schedule>   sch,
        const std::vector<MGEN::InferObject>& abnormal_results,
        const tm*                             time_info,
        const std::string&                    image_path
    )
    {
        // Set const
        const int         CamId      = this->id_;
        const int         EventCode  = static_cast<int>( sch->sch_info.event_code );
        const int         LevelCode  = sch->sch_info.level_code;
        const int         ScheduleId = sch->sch_info.schedule_id;
        const int         ServerId   = this->socket_io_server_id_;
        const std::string ImagePath  = image_path;
        const std::string StrUUID    = UUID::GetGenerator()->generate();
        const int         TrackId    = ( abnormal_results.size() ) ? abnormal_results.front().track_id : NON_TRACKING_IDX;

        // Get time from tm* & formatting
        char szTime[20] = {'\0', };
        std::strftime( szTime, sizeof(szTime), "%Y%m%d_%H%M%S", time_info );
        const std::string StrTime { szTime };

        // Set UID
        const std::string Uid
            = StrTime
            + "_" + std::to_string( CamId )
            + "_" + std::to_string( EventCode )
            + "_" + StrUUID;

        // Set Objects
        json Objects = json::array();
        for( const auto& result : abnormal_results )
        {
            // Set
            json object = json::object();
            object["Confidence"]  = result.score;
            object["ClassId"]     = result.class_id;
            object["TrackId"]     = result.track_id;
            object["VideoWidth"]  = stable_frame_w_;
            object["VideoHeight"] = stable_frame_h_;
            object["Box"]         = {
                static_cast<int>(result.bbox.x),
                static_cast<int>(result.bbox.y),
                static_cast<int>(result.bbox.x + result.bbox.w),
                static_cast<int>(result.bbox.y + result.bbox.h)
            };

            // 이벤트 타입에 맞는 Extend Data(Count, Duration, Direction) 주입
            MGEN::Abnormal::Schedule::SetExtendedDataToJson( object, result, sch->sch_info.event_code );

            // Insert
            Objects.push_back( std::move( object ) );
        }

        // Build default json
        json EventNotify = this->BuildNotifyJsonBase(
            GetEventNameStringFromEventNameEnumValue( SocketIO::EventName::DETECTOR::EventNotifycation ),
            SocketIO::TransmissionType::BroadCast, time_info, std::string {""}
         );

        // Build sub-detail json
        json Analysis          = json::object();
        Analysis["Time"]       = EventNotify["Time"].get<string>();
        Analysis["Uid"]        = Uid;
        Analysis["TrackId"]    = TrackId;
        Analysis["CameraId"]   = CamId;
        Analysis["EventCode"]  = EventCode;
        Analysis["LevelCode"]  = LevelCode;
        Analysis["ScheduleId"] = ScheduleId;
        Analysis["ServerId"]   = ServerId;
        Analysis["ImagePath"]  = ImagePath;
        Analysis["Objects"]    = Objects;
        Analysis["Confirm"]    = 0;
        Analysis["Checker"]    = "";

        EventNotify["Data"]["Message"]["Analysis"] = Analysis;

        return EventNotify;
    }

    bool RtspDetectorUnit::IsLoadable( std::string_view target_engine_magic_name ) const
    {
        if( !this->load_balancer_ )
            return false;

        auto target_id = load_balancer_->GetAvailableInferEngineID( std::string( target_engine_magic_name ) );
        return ( INFER_ENGINE_ID_NOT_SET != target_id );
    }

    void RtspDetectorUnit::InferenceThreadRunner( void )
    {
        // P53: 이 thread 의 모든 MLOG 에 정적 correlation_id 부여.
        // format: "sys-detector-<cam_id>" — 카메라별 thread 식별자.
        MGEN::CorrelationContext::Set( "sys-detector-" + std::to_string( this->id_ ) );

        // ---------------------------------------------------------------------
        // 1. Engine Client Setup (단일 Detection 엔진)
        // ---------------------------------------------------------------------
        std::vector<EngineClient> detection_engines;
        SwsContextManager         context_manager;
        FrameFormattingContext    origin_ctx; // for origin size image formatting

        // 매직 이름이 일치하는 Detection 엔진 1개만 로드.
        // Odroid M2 NPU 환경에서는 멀티엔진 동시 로드가 불가능하므로 단일 구성.
        const bool is_loadable_person = this->IsLoadable( MAGIC_DETECTION_ENGINE_NAME );

        // ---------------------------------------------------------------------
        // Set class id at specific engine, for shedule class checker
        // ---------------------------------------------------------------------
        // MGEN::Schedule 등에서 클래스 아이디를 범용 판별할 때의 기준값.
        InferClassID class_id_person = INFER_CLASS_ID_NOT_SET;
        InferClassID class_id_car    = INFER_CLASS_ID_NOT_SET;

        // ---------------------------------------------------------------------
        // Detection 엔진 EngineClient Context 세팅
        // ---------------------------------------------------------------------
        if( is_loadable_person )
        {
            EngineClient client;

            bool success = client.Init
            (
                // For create engine request meta data
                id_, load_balancer_,

                // String Name Tag ( magic )
                // 외부 엔진 profile.json에 적힌 MagicName이 이것과 동일해야 찾아짐
                MAGIC_DETECTION_ENGINE_NAME,

                // Int engine code ( from EngineStreamTypes.h )
                ENGINE_MAGIC_TYPE_DETECTION,

                // 이 엔진에서 검출할 클래스명의 목록 (COCO 80 클래스 기준 Person/Car)
                {"Person", "Car"},

                // 단일 엔진 구성이므로 RGB 단일 모드로 항상 활성.
                []( const EngineActiveContext& /*ctx*/ ) { return true; }
            );

            if( success )
            {
                class_id_person = client.GetClassID( "Person" );
                class_id_car    = client.GetClassID( "Car" );

                detection_engines.push_back( std::move(client) );
            }
            else { MLOG_ERROR("CAM[%d] Failed to init DetectionEngine", id_); }
        }

        // ---------------------------------------------------------------------
        // Subscribe All Detection Engine
        // ---------------------------------------------------------------------
        for( const auto& eng : detection_engines ){
            subscribe_ids_.insert( eng.subscribe_id );
        }
        for( const auto& sid : subscribe_ids_ ){
            this->load_balancer_->Subscribe( sid );
        }

        // ---------------------------------------------------------------------
        // 2. Loop
        // ---------------------------------------------------------------------
        // Warm up
        // Main 쓰레드 진입 전 RTSP Proxy 최초 정보 수신 부분
        auto& running = this->inference_thread_.GetRunningFlag();
        do
        {
            proxy_ptr_ = rtsp_handler_->GetProxyPtr( id_ );
            std::this_thread::sleep_for( 100ms );
        }
        while( running.load() == true && proxy_ptr_ == nullptr );

        // exit early
        if( running.load() == false )
            return;

        // Link queue ( RTSP - DetectImpl(here) )
        proxy_ptr_->setDecodedFrameSafeQueue( avframe_q_, true, detect_fps_limit_ );

        // const
        const auto dequeue_timeout                = 100ms;
        const int  inference_wait_ms              = 5000;  // 5sec
        const auto long_timelapse_log_interval = 10min; // 10min
        const auto fps_update_interval            = 60s;

        // set thread internal timer
        EventTime avframe_current_recv_time     = std::chrono::system_clock::now();
        EventTime last_interval_log_print_time  = std::chrono::system_clock::now();
        EventTime last_interval_update_fps_time = std::chrono::system_clock::now();

        // reset members
        this->consecutive_mismatch_count_ = 0;

        // Main
        while( running.load() == true )
        {
            // Read AVFrame from rtsp_proxy(CRtspProxy)
            // dequeue_wait_for 는 종료/타임아웃 시 std::nullopt 반환 (throw 없음)
            std::optional<std::shared_ptr<AVFrame>> opt_frame = avframe_q_->dequeue_wait_for( dequeue_timeout );

            if( opt_frame.has_value() == false )
            {
                // Case 1. terminate
                if( avframe_q_->is_terminated() ){
                    continue;
                }
                // Case 2. timeout
                auto current_check_time = std::chrono::system_clock::now();
                if( IsOverTime( avframe_current_recv_time,    current_check_time, long_timelapse_log_interval ) &&
                    IsOverTime( last_interval_log_print_time, current_check_time, long_timelapse_log_interval ) ){
                    MLOG_INFO("Cam[%d] decoded frame not recieved... maybe RTSP stream not activate", id_ );
                    last_interval_log_print_time = current_check_time;
                }
                continue;
            }
            else
            {
                // check frame recv time
                avframe_current_recv_time = std::chrono::system_clock::now();
            }

            // get AVFrame ptr
            std::shared_ptr<AVFrame> frame = *opt_frame;

            // 엔진 로드가 아직 이루어지지 않았다면 스킵
            if( load_balancer_->IsLoadEngine() == false ) {
                std::this_thread::sleep_for( 10ms );
                continue;
            }

            // 예외 카메라면 스킵 (callback 측 쓰기와 동시성 race 방지: mutex 잡고 검사)
            bool is_excluded = false;
            {
                std::lock_guard<std::mutex> lck { this->exclude_setting_mtx_ };
                is_excluded = !exclude_setting_.IsEmpty();
            }
            if( is_excluded ) {
                std::this_thread::sleep_for( 10ms );
                continue;
            }

            // 최초 실행 시 안정화 해상도 초기화
            if( stable_frame_w_ == 0 || stable_frame_h_ == 0 )
            {
                stable_frame_w_ = frame->width;
                stable_frame_h_ = frame->height;
            }

            // Tracker & Schedule reset flag set, default = false
            bool need_reset_schedule_n_tracker_cuz_resize = false;

            // 현재 프레임이 안정화된 해상도와 다른지 검사
            if( frame->width != stable_frame_w_ || frame->height != stable_frame_h_ )
            {
                consecutive_mismatch_count_++;

                // 아직 임계값 미만이면 노이즈로 간주하고 스킵
                if( consecutive_mismatch_count_ < REINIT_THRESHOLD_COUNT ){
                    MLOG_WARN("CAM[%d] Frame size mismatch (Noise?). Count: %zu", id_, consecutive_mismatch_count_);
                    continue;
                }

                // 임계값 초과 -> 실제 해상도 변경으로 인정
                MLOG_WARN("CAM[%d] Resolution changed (%dx%d -> %dx%d). Re-initializing contexts.", id_, stable_frame_w_, stable_frame_h_, frame->width, frame->height);

                stable_frame_w_ = frame->width;
                stable_frame_h_ = frame->height;

                // sws Context 매니저 초기화
                context_manager.ClearAll();

                // set flag
                need_reset_schedule_n_tracker_cuz_resize = true;

                // reset count
                consecutive_mismatch_count_ = 0;
            }
            else
            {
                // 연속성 중간에 해상도 한 번이라도 일치하다면 카운터 리셋
                consecutive_mismatch_count_ = 0;
            }

            int  last_realtime_fps = realtime_fps_;
            bool need_update_fps   = false;

            if( IsOverTime( last_interval_update_fps_time, avframe_current_recv_time, fps_update_interval ) )
            {
                if( proxy_ptr_ ){
                    auto opt_fps = proxy_ptr_->getRealtimeFps();
                    if( opt_fps.has_value() )
                    {
                        int curr_realtime_fps = static_cast<int>( std::round( *opt_fps ) );

                        // FPS 변화가 5 이상일 때만 업데이트
                        if( std::abs( curr_realtime_fps - last_realtime_fps ) >= 5 )
                        {
                            realtime_fps_   = curr_realtime_fps;
                            need_update_fps = true;

                            MLOG_INFO("CAM[%d] Realtime FPS check: %d -> %d ( real : %.2f )",
                                id_, last_realtime_fps, curr_realtime_fps, *opt_fps );
                        }
                    }
                }
                last_interval_update_fps_time = avframe_current_recv_time;
            }

            // Schedule update
            if( this->is_schedule_updated_.load() || need_reset_schedule_n_tracker_cuz_resize || need_update_fps )
            {
                this->ReleaseSchedules();

                // capture current schedule setting atomatic
                std::unique_lock<std::mutex> lck { this->schedule_settings_mtx };
                auto schedule_capture = this->schedule_settings_;
                this->is_schedule_updated_.store(false);
                lck.unlock();

                // need reset tracker too
                if( need_reset_schedule_n_tracker_cuz_resize ){
                    this->ResetTrackers();
                }

                // set lambda for event_class_checker for inner schedule
                ClassEqualChecker event_class_checker
                    = [ class_id_person, class_id_car ] ( ClassIntID id, const ClassTagName& tag ) -> bool {
                        const std::string upper = MGEN::ToUpperCase( tag );
                        if      ( upper == std::string { "PERSON" } && INFER_CLASS_ID_NOT_SET != class_id_person ) { return ( id == class_id_person ); }
                        else if ( upper == std::string { "CAR"    } && INFER_CLASS_ID_NOT_SET != class_id_car    ) { return ( id == class_id_car    ); }
                        else {
                            return false;
                        }
                    };

                for( const auto& sch : schedule_capture.schedules )
                {
                    int fps_max = std::min( realtime_fps_, detect_fps_limit_ );

                    scheduler_.push_back(
                        std::make_shared<Abnormal::Schedule>( sch, stable_frame_w_, stable_frame_h_, fps_max, event_class_checker )
                    );

                    MLOG_INFO( "Schedule Update ( ID - %05d ) => CAM [%04d] : %-18s",
                        sch.schedule_id, id_, Abnormal::Schedule::GetEventName( sch.event_code ).c_str() );
                }
            }

            // update origin sws formatting scale context
            if( !origin_ctx.Update( frame->width, frame->height, frame->width, frame->height, frame->format ) ){
                MLOG_ERROR("CAM[%d] Context update failed for origin size.", id_);
            }

            // Identify Active Engines & Unique resolutions (단일 RGB 엔진 구성)
            EngineActiveContext active_ctx;
            active_ctx.color_mode = CameraColorMode::RGB;

            std::vector<EngineClient*> active_detection_engines_ptr;
            std::set<ResolutionKey>    required_resolutions;

            // 활성화된 엔진 선별 및 필요 해상도 수집
            for( auto& engine : detection_engines )
            {
                if( engine.is_active_condition( active_ctx ) ){
                    active_detection_engines_ptr.push_back( &engine );
                    required_resolutions.insert( { engine.input_width, engine.input_height } ); // ex) 640, 640
                }
            }

            if( active_detection_engines_ptr.empty() ){
                std::this_thread::sleep_for( 10ms );
                continue;
            }

            // Preprocessing
            bool preprocessing_success = true;

            // 필요한 각 해상도에 대해 딱 한 번씩만 변환 수행
            // 여기서 검색은 수행하지 않음. 이상 행동 발생 시에만 검색 엔진 활성화.
            for( const auto& res_key : required_resolutions )
            {
                FrameFormattingContext* ctx = context_manager.GetContext( res_key.w, res_key.h, frame );

                // Context update ( for calc Letterbox and allocate buffer )
                if( !ctx->Update( frame->width, frame->height, res_key.w, res_key.h, frame->format ) ){
                    MLOG_ERROR("CAM[%d] Context update failed for target size %dx%d", id_, res_key.w, res_key.h);
                    preprocessing_success = false;
                    break;
                }

                // RKNN/NPU 환경: 엔진 반환값이 ratio가 아닌 pixel 형태이므로 명시 설정
                ctx->inference_style.format = BBoxCoordinateFormat::pixel_int;

                if( ctx->Convert( frame ) == false ){
                    MLOG_ERROR("CAM[%d] frame convert failed.", id_);
                    preprocessing_success = false;
                    break;
                }

                if( ctx->IsReadyForInference() == false ){
                    MLOG_ERROR("CAM[%d] frame convert, but result invalid for inference", id_);
                    preprocessing_success = false;
                    break;
                }
            }

            if( !preprocessing_success )
                continue;

            uint64_t current_correlation_id = this->frame_count_.fetch_add(1);

            // 요청에 성공한 엔진들만 따로 모을 리스트
            std::vector<EngineClient*> requested_engines;
            requested_engines.reserve( active_detection_engines_ptr.size() );

            // Build req data & Request
            for( auto* engine : active_detection_engines_ptr )
            {
                // 이미지가 준비된 Context를 가져옴 (연산 비용 0)
                FrameFormattingContext* ctx = context_manager.GetContext( engine->input_width, engine->input_height, frame );

                // Request ( Asnc )
                // 반환값을 확인하여 성공한 경우에만 대기 목록에 추가
                bool req_success = load_balancer_->RequestAsync( std::move( engine->BuildRequest( ctx, current_correlation_id ) ) );

                if ( req_success )
                {
                    requested_engines.push_back( engine );
                }
                // 로드밸런서가 Backpressure로 인해 요청을 거부했다면 대기하지 않음 (Hang 방지)
            }

            if( requested_engines.empty() ){
                // 모두 요청이 드랍된 경우
                continue;
            }

            // Wait & Merge
            std::vector<InferObject> merged_results;

            // Budget 관리용 시작 시간 기록
            auto wait_start_time = std::chrono::steady_clock::now();

            // 불필요한 std::future/async 벡터 제거. 바로 루프를 돌며 수거합니다.
            for( auto* engine : requested_engines )
            {
                // 1. 현재 소모된 시간을 계산하여 남은 Budget(ms) 산출
                auto now              = std::chrono::steady_clock::now();
                auto elapsed_ms       = std::chrono::duration_cast<std::chrono::milliseconds>(now - wait_start_time).count();
                int  remaining_budget = inference_wait_ms - static_cast<int>(elapsed_ms);

                // 2. 만약 이미 5초를 다 썼다면 더 이상 기다리지 않고 다음 엔진으로 skip (로그는 남김)
                if( remaining_budget <= 0 )
                {
                    MLOG_WARN("CAM[%d] Engine %s skipped (Total budget %dms exhausted)",
                        id_, engine->magic_name.c_str(), inference_wait_ms );
                    continue;
                }

                // 3. 남은 시간만큼만 RespondAsync 대기 (스레드 생성 없이 직접 호출)
                // 이미 결과가 도착해 있다면 지연 없이 즉시 반환됩니다.
                auto result_opt = load_balancer_->RespondAsync( engine->subscribe_id, remaining_budget );

                if( result_opt.has_value() )
                {
                    // for DFPS check
                    load_balancer_->AddInferCount( engine->subscribe_id );

                    // Context 가져오기
                    auto* ctx = context_manager.GetContext( engine->input_width, engine->input_height, frame );

                    for( const auto& obj : result_opt->infer_objects )
                    {
                        bool target_found = false;
                        for( auto const& [ name, id ] : engine->target_class_ids )
                        {
                            if( id == obj.class_id ){
                                target_found = true;
                                break;
                            }
                        }

                        if( target_found )
                        {
                            InferObject target = obj;
                            ConvertInferObjectCoordinate( target, ctx->inference_style, ctx->original_style );
                            merged_results.push_back( target );
                        }
                    }
                }
                else
                {
                    // RespondAsync 내부에서 타임아웃 발생 시
                    MLOG_WARN("CAM[%d] Engine %s timeout (Waited max %d ms)",
                        id_, engine->magic_name.c_str(), remaining_budget);
                }
            }

            // Tracking
            std::vector<InferObject> entire_track_results;
            std::map<InferClassID, std::vector<InferObject>> classified_objects;

            // split
            for( const auto& obj : merged_results ){
                classified_objects[obj.class_id].push_back( obj );
            }

            for( auto& [ class_id, objects ] : classified_objects )
            {
                const bool need_tracking
                    = ( class_id == class_id_person ) || ( class_id == class_id_car );

                if( need_tracking )
                {
                    if( trackers_.find( class_id ) == trackers_.end() )
                    {
                        // original size tracker
                        ImageExpressStyle tracker_style( frame->width, frame->height );

                        // create tracker
                        trackers_[class_id]     = std::make_unique<SORTTracker>( tracker_style, tracker_style );
                        tracker_seqs_[class_id] = 1;
                    }

                    auto& seq = tracker_seqs_[class_id];
                    auto  res = trackers_[class_id]->TrackObjects( objects, seq++ );

                    if( res ){
                        entire_track_results.insert( entire_track_results.end(), res->begin(), res->end() );
                    }
                }
                else
                {
                    entire_track_results.insert( entire_track_results.end(), objects.begin(), objects.end() );
                }
            }

            SendDetectResultToMetaData( entire_track_results );

            if( entire_track_results.size() == 0 )
                continue;

            if( scheduler_.empty() ){
                if( IsOverTime( avframe_current_recv_time, last_interval_log_print_time, long_timelapse_log_interval ) ){
                    MLOG_INFO("CAM[%d] AI inference detected ( %4d ojbect ), but target camera has not exist EVENT schedule.", id_, entire_track_results.size() );
                    last_interval_log_print_time = avframe_current_recv_time;
                }
                continue;
            }

            // Set
            std::vector<ScheduleEventDTO> event_list {};
            std::string frame_path {};

            // Reserve
            event_list.reserve( scheduler_.size() );
            frame_path.reserve( 128 );

            if( auto opt = MakeImageSavePath( GetFrameImageCurrentProxyRootPath() ); opt.has_value() ){
                frame_path = *opt;
            }
            else {
                MLOG_ERROR( "%s() => CAM[%d] make frame image save directory failed", __func__, id_ );
                continue;
            }

            // time get for socket.io emit
            struct tm tstruct;
            auto      curr_time = std::time( nullptr );
            auto*     time_info = localtime_r( &curr_time, &tstruct );

            // Loop schedules
            for( const auto& each_schedule : scheduler_ )
            {
                // calculate abnormal action algorithm here
                const std::vector<InferObject> on_event_results = each_schedule->Check( entire_track_results );

                if( on_event_results.empty() ){
                    continue;
                }

                // P54: 이벤트 감지 누적 (Prometheus exporter) + 로그 한 줄.
                {
                    const std::string ev_name = Abnormal::Schedule::GetEventName( each_schedule->sch_info.event_code );
                    MGEN::MetricsRegistry::Instance().IncrementCounter(
                        "detectbase_events_total",
                        { { "type", ev_name }, { "cam", std::to_string( id_ ) } },
                        static_cast<double>( on_event_results.size() ) );

                    // correlation_id 는 sys-detector-{cam} 자동 첨부.
                    MLOG_INFO( "event_detected type=%s cam=%d count=%zu",
                        ev_name.c_str(), id_, on_event_results.size() );
                }

                // Build socket.io message
                auto event_msg_json = this->BuildNotifyJsonImpl_Analysis( each_schedule, on_event_results, time_info, frame_path );

                // cache
                event_list.push_back( ScheduleEventDTO{ std::move( event_msg_json ), on_event_results } );
            }

            if( event_list.empty() ){
                continue;
            }

            // origin size cv::Mat
            if( origin_ctx.Convert( frame, true, false ) == false ){
                MLOG_ERROR("CAM[%d] Postprocess | Origin Image Convert failed", id_);
                continue;
            }

            // cv::imwrite 는 IO Worker thread 에 위임 (main loop 의 frame drop 차단).
            // origin_ctx.save_snapshot_mat 는 다음 frame 의 Convert() 에서 재사용되므로 deep copy (clone) 필수.
            if( io_work_queue_ ){
                IOWorkItem item;
                item.frame_path = frame_path;
                item.image_mat  = origin_ctx.save_snapshot_mat.clone();
                // 운영 가시성: 큐 가득 시 drop oldest (max=30). 통계 메트릭 race 영향 미미.
                if( io_work_queue_->size() >= 30 ) {
                    MGEN::MetricsRegistry::Instance().IncrementCounter(
                        "detectbase_errors_total", { { "type", "io_work_drop" } } );
                }
                io_work_queue_->enqueue_move( std::move( item ) );
            }

            // Emit 은 main 즉시 호출 — sio emit_queue 가 이미 비동기 (~1ms enqueue).
            // 이벤트 알림 latency 짧음 + cv::imwrite 분리와 무관.
            if( sio_handler_ )
            {
                for( auto& target_event : event_list )
                {
                    sio_handler_->Emit( std::string { SocketIO::EventName::DETECTOR_MESSAGE }, target_event.event_message );
                }
            }

            // GRPC client (Phase 1) — 활성 peer 모두에게 fire-and-forget push.
            // 비활성 시 BroadcastEventOnlyJsonToGrpcPeers 가 즉시 0 반환 (no-op).
            if( network_manager_ && network_manager_->IsGrpcClientEnabled() )
            {
                for( auto& target_event : event_list )
                {
                    const size_t sent = network_manager_->BroadcastEventOnlyJsonToGrpcPeers(
                        target_event.event_message.dump() );
                    if( sent > 0 ) {
                        MGEN::MetricsRegistry::Instance().IncrementCounter(
                            "detectbase_grpc_send_total",
                            { { "rpc", "SendEventOnlyJson" } },
                            static_cast<double>( sent ) );
                    }
                }
            }

        } // while running true

    }

    void RtspDetectorUnit::InferenceThreadCloser( void )
    {
        for( const auto& subs_id : subscribe_ids_ ){
            load_balancer_->Unsubscribe( subs_id );
        }

        avframe_q_->terminate();
        avframe_q_->clear_without_action();
    }

    void RtspDetectorUnit::IOWorkerThreadRunner( void )
    {
        // P53: 이 thread 의 모든 MLOG 에 정적 correlation_id 부여.
        MGEN::CorrelationContext::Set( "sys-io_worker-" + std::to_string( this->id_ ) );

        using namespace std::chrono_literals;
        auto& running = this->io_worker_thread_.GetRunningFlag();

        // P54 Layer 1: cool-down WARN 로그 (1분에 한 번만 출력).
        std::chrono::steady_clock::time_point last_skip_warn {};

        // P54 Layer 2-Regular: 카메라별 자동 청소 주기. thread-local 시각 비교로 약 1시간 1회.
        std::chrono::steady_clock::time_point last_cleanup {};

        // P54 Layer 2-Emergency: 디스크 사용률 >= 80% 시 과거 날짜 우선 / 당일만 남으면 절반 삭제.
        // 5분 cool-down — 비상 청소가 매번 dequeue 마다 호출되지 않게.
        std::chrono::steady_clock::time_point last_emergency {};

        while( running.load() == true )
        {
            // 종료/타임아웃 시 std::nullopt 반환 (throw 없음).
            auto opt_item = io_work_queue_->dequeue_wait_for( 1s );

            // P54 Layer 2-Regular: 자동 청소 (1시간 1회) — 7일 이전 일자 폴더 삭제.
            const auto now = std::chrono::steady_clock::now();
            if( now - last_cleanup > FRAME_CLEANUP_INTERVAL ) {
                last_cleanup = now;
                const size_t deleted = CleanupOldFrameDirs( FRAME_RETENTION_DAYS );
                if( deleted > 0 ) {
                    MLOG_INFO( "CAM[%d] IOWorker: cleanup deleted %zu day-folders (>= %d days old)",
                        id_, deleted, FRAME_RETENTION_DAYS );
                    MGEN::MetricsRegistry::Instance().IncrementCounter(
                        "detectbase_frame_cleanup_deleted_total", {},
                        static_cast<double>( deleted ) );
                }
            }

            // P54 Layer 2-Emergency: 디스크 사용률 >= 80% 시 비상 청소 (5분 cool-down).
            // 정기 청소와 무관하게 작동 — 디스크 빠르게 차오를 때 안전 장치.
            if( now - last_emergency > FRAME_EMERGENCY_INTERVAL ) {
                last_emergency = now;
                const auto res = EmergencyCleanupIfDiskHigh();
                if( res.deleted_day_dirs > 0 || res.deleted_files > 0 ) {
                    MLOG_WARN( "CAM[%d] IOWorker: EMERGENCY cleanup — deleted %zu day-folders + %zu files (disk >= %.1f%%)",
                        id_, res.deleted_day_dirs, res.deleted_files, FRAME_DISK_EMERGENCY_PCT );
                    auto& m = MGEN::MetricsRegistry::Instance();
                    if( res.deleted_day_dirs > 0 ) {
                        m.IncrementCounter( "detectbase_frame_emergency_cleanup_total",
                            { { "type", "day_dir" } },
                            static_cast<double>( res.deleted_day_dirs ) );
                    }
                    if( res.deleted_files > 0 ) {
                        m.IncrementCounter( "detectbase_frame_emergency_cleanup_total",
                            { { "type", "half_files" } },
                            static_cast<double>( res.deleted_files ) );
                    }
                }
            }

            if( !opt_item.has_value() ){
                continue; // 종료 또는 타임아웃 → running 재검사
            }

            // P54 Layer 1: imwrite 직전 사전 차단. 디스크 가득 차면 skip.
            const double used_pct = GetFrameDiskUsedPercent();
            if( used_pct >= FRAME_DISK_FULL_PCT ) {
                MGEN::MetricsRegistry::Instance().IncrementCounter(
                    "detectbase_imwrite_skipped_total", { { "reason", "disk_full" } } );

                if( now - last_skip_warn > std::chrono::minutes( 1 ) ) {
                    MLOG_WARN( "CAM[%d] IOWorker: imwrite skipped — frame disk %.1f%% used (>= %.1f%% threshold)",
                        id_, used_pct, FRAME_DISK_FULL_PCT );
                    last_skip_warn = now;
                }
                continue; // imwrite 안 함
            }

            // cv::imwrite (~50-200ms). main loop 에 영향 없음.
            if( !cv::imwrite( opt_item->frame_path, opt_item->image_mat ) ){
                MLOG_ERROR( "CAM[%d] IOWorker: failed to save output frame: %s",
                            id_, opt_item->frame_path.c_str() );
                // P54: imwrite 실패 누적.
                MGEN::MetricsRegistry::Instance().IncrementCounter(
                    "detectbase_errors_total", { { "type", "imwrite_fail" } } );
            }
        }
    }

    void RtspDetectorUnit::IOWorkerThreadCloser( void )
    {
        // dequeue_wait_for 가 std::nullopt 반환하도록 terminate.
        // 큐에 남은 work item 은 자연 drop (graceful shutdown 시 imwrite 누락 가능 — 의도됨).
        if( io_work_queue_ ){
            io_work_queue_->terminate();
            io_work_queue_->clear_without_action();
        }
    }

    void RtspDetectorUnit::ReleaseSchedules( void )
    {
        this->scheduler_.clear();
    }

    void RtspDetectorUnit::ResetTrackers( void )
    {
        this->trackers_.clear();
        this->tracker_seqs_.clear();
    }

    // RtspDetectorUnit
} // namespace MGEN
