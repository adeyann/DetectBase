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
#include <dlfcn.h>             // jemalloc mallctl symbol dlsym 동적 로딩
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
// W-14 (DEPRECATED 2026-05-16): jemalloc 도입 후 malloc_trim(0) 이 사실상 no-op
//   라 활성 코드에서 제거했다. 호출이 다시 필요해지면 아래 헤더와 §emergency
//   cleanup 의 주석 처리된 한 줄을 함께 복구할 것.
// #include <malloc.h>          // glibc malloc_trim: emergency cleanup 후 heap 강제 반환

// ---------------------------------------------------------------------------
// DBG_PROF — file-local instrumentation wrapping (v0.1.20).
// 측정 코드 (stage timing / jemalloc mallctl / /proc/self/maps / 100-cycle
// dump) 가 thread function 안에 50+ 곳 인터리브되어 있어 매크로 1개로 압축.
// Release 빌드 (DEBUG_MODE off) 에선 빈 expansion → preprocessor 단계에서
// 제거 → 0 runtime cost + 0 binary footprint. 큰 블록은 #ifdef DEBUG_MODE
// 직접 wrap 으로 결합.
// ---------------------------------------------------------------------------
#ifdef DEBUG_MODE
    #define DBG_PROF( ... ) __VA_ARGS__
#else
    #define DBG_PROF( ... )
#endif

namespace MGEN
{
    using namespace std::chrono_literals;
    using nlohmann::json;

    inline static bool IsOverTime( const MGEN::EventTime& t1, const MGEN::EventTime& t2, std::chrono::milliseconds lapse )
    {
        const auto diff = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t2).count();
        return std::abs(diff) > lapse.count();
    }

    // correlation_mismatch_total metric registration (PR #9 결함 fix 2026-05-20).
    //   IncrementCounter (line 1442) 는 unregistered metric 호출 시 silently no-op 이라
    //   PR #9 이후 4882건 발생한 mismatch 가 metric 으로 노출 안 됐었음.
    //   ctor 에서 std::call_once 로 한 번만 register.
    namespace
    {
        std::once_flag g_correlation_mismatch_metric_once;
        void RegisterCorrelationMismatchMetricOnce() noexcept
        {
            std::call_once( g_correlation_mismatch_metric_once, []{
                MGEN::MetricsRegistry::Instance().RegisterCounter(
                    "detectbase_correlation_mismatch_total",
                    "Correlation ID mismatch between inflight frame and NPU response per cam_id (frame ordering defense counter)" );
            } );
        }
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
                // raw loop 가독성 우선.
                // cppcheck-suppress useStlAlgorithm
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

            // W-14 (DEPRECATED 2026-05-16): glibc malloc 의 heap arena 를 OS 로 강제 반환.
            //   cleanup 안의 std::filesystem path string 할당/해제 burst 로 인한
            //   fragmentation 누적 회피용. 48h 테스트 시 후반 10h 에서 RssAnon
            //   +47 MB 증가 관측되어 도입했음.
            //   jemalloc (LD_PRELOAD, 2026-05-14 도입) 환경에서는 사실상 no-op:
            //   - malloc_trim 은 glibc 전용 — jemalloc 이 wrap export 안 함
            //   - application 의 모든 malloc 이 jemalloc 으로 → glibc heap 거의 비어있음
            //   - 실제 page 회수는 jemalloc background_thread (MADV_DONTNEED) 가 담당
            //   다시 활성화하려면 위 #include <malloc.h> 도 함께 복구할 것.
            // malloc_trim( 0 );

            return res;
        }
    } // namespace

    // [DEBUG VIRTUAL LINES] 디버깅 / 시연 / 이벤트 빈발 시뮬레이션 목적.
    // ServerSetting `debug_virtual_lines_enabled` (default false) 가 true 일 때만 호출됨.
    // 모든 카메라에 가상 schedule 2개 추가:
    //   - ID 99999: LineIntrusion (사람) — 가로 3 + 세로 3 = 6개 라인
    //   - ID 99998: VehicleIntrusion (차량) — 동일 6개 라인
    // 사용법: README.md §14 참조.
    static void AddDebugVirtualLines( ScheduleSettingData& data ) noexcept
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

    RtspDetectorUnit::RtspDetectorUnit
    (
        const MGEN::Type::UnitID            service_unit_id,
        const ServiceBlockProfile&          /*service_block_profile*/,
        std::shared_ptr<NetworkManager>     network_manager,
        std::shared_ptr<IOStreamManager>    io_stream_manager,
        std::shared_ptr<EngineLoadBalancer> load_balancer
    )
        : id_              ( service_unit_id   )
        , load_balancer_   ( std::move( load_balancer )     )
        , network_manager_ ( std::move( network_manager )   )
        , iostream_manager_( std::move( io_stream_manager ) )
    {
        RegisterCorrelationMismatchMetricOnce();
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

        // B2 async pipeline — InferenceThread (producer) 와 ResponseThread (consumer) 사이의 inflight queue.
        // cam thread 가 RequestAsync 후 inflight push, response thread 가 dequeue 후 RespondAsync + post.
        //
        // cap=10 결정 근거 (2026-05-18 v6 monitor 분석):
        //   - RspThread total cycle ≈ cam interval (~34ms). pop rate ≈ push rate → drain rate 0.
        //   - jitter spike (EOS reconnect 등) 마다 backlog +N 누적. spike 후에도 drain 못 함.
        //   - cap=60 으로 5h 가동 시 한 cam 의 inflight 가 cap 까지 도달 (CAM[661] 58/60 직전).
        //   - 60 frame × 3MB × 4 cam = 720 MB anon 누적. baseline 크게 형성.
        //   - cap=10 으로 줄이면 drop_oldest 일찍 발동 (q_drop inf > 0) → 누적 폭 10 × 3MB × 4 cam = 120 MB 만.
        //   - dfps 영향 없음: drop_oldest 가 oldest frame 만 버리고 NPU 가 항상 latest 처리.
        //   - tracking continuity 약간 영향 가능 — 진짜 fix (B4 = RspThread cycle < cam interval) 는 별도 작업.
        this->inflight_q_ = std::make_shared<SafeQueue<InflightItem>>();
        this->inflight_q_->SetMaxSize( 10 );
        this->response_thread_.SetThreadFunctions(
            std::bind( &RtspDetectorUnit::ResponseThreadRunner, this ),
            std::bind( &RtspDetectorUnit::ResponseThreadCloser, this )
        );

        // B3 — ResponseThread (producer) 와 EventThread (consumer) 사이의 event queue.
        // RspThread 가 tracking 결과 push, EvtThread 가 schedule check + sio/grpc + io_work_queue 처리.
        // EventItem = frame (shared_ptr ref) + track_results (vector). frame 보존 비용 = shared_ptr ref 1 (~bytes).
        // cap=10 — event 빈도 평균 1/min 미만, burst 도 ~수 event/sec 이내 → 10 충분. drop_oldest 발동 시 oldest event 버림.
        this->event_q_ = std::make_shared<SafeQueue<EventItem>>();
        this->event_q_->SetMaxSize( 10 );
        this->event_thread_.SetThreadFunctions(
            std::bind( &RtspDetectorUnit::EventThreadRunner, this ),
            std::bind( &RtspDetectorUnit::EventThreadCloser, this )
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
        // [DEBUG VIRTUAL LINES] ServerSetting toggle 기반 — enable 시 모든 카메라에 가상 boundary 주입.
        // schedule 없는 카메라에도 toggle ON 이면 가상 라인 적용 → lock 항상 잡고 처리.
        {
            std::lock_guard<std::mutex> sch_lck { this->schedule_settings_mtx };
            if( auto schedule_data_opt = sm->GetScheduleSetting( id_ ); schedule_data_opt.has_value() ) {
                this->schedule_settings_ = *schedule_data_opt;
            }
            if( auto srv_opt = sm->GetServerSetting(); srv_opt.has_value() && srv_opt->debug_virtual_lines_enabled ) {
                AddDebugVirtualLines( this->schedule_settings_ );
                MLOG_INFO( "CAM[%d] debug_virtual_lines_enabled=true — schedule 99999 (LineIntrusion) + 99998 (VehicleIntrusion) 강제 주입", static_cast<int>( this->id_ ) );
            }
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
            [this, sm](const ScheduleSettingData& newData)
            {
                std::lock_guard<std::mutex> lck { this->schedule_settings_mtx };
                this->schedule_settings_ = newData;
                if( auto srv_opt = sm->GetServerSetting(); srv_opt.has_value() && srv_opt->debug_virtual_lines_enabled ) {
                    AddDebugVirtualLines( this->schedule_settings_ );
                }
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

        // B3 — Event thread 가 가장 downstream consumer. response 가 event_q push 시 이미 가동되어야 함.
        if( this->event_thread_.Start() == false )
            return false;

        // B2 — Response thread 도 inference 시작 전에 start (inflight push 시 consumer 준비).
        if( this->response_thread_.Start() == false )
            return false;

        if( this->inference_thread_.Start() == false )
            return false;

        return true;
    }

    bool RtspDetectorUnit::Stop( void )
    {
        // !!! DO NOT REORDER !!! Shutdown 순서 (B2+B3 async pipeline):
        //   1) inference_thread Stop  — producer 정지 → inflight push 없음
        //      InferenceThreadCloser 가 inflight_q_->terminate() 호출 (response_thread 가 dequeue 종료 가능)
        //   2) response_thread Stop  — RspThread 정지 → event_q push 없음
        //      ResponseThreadCloser 가 event_q_->terminate() 호출 (event_thread 가 dequeue 종료 가능)
        //   3) event_thread Stop  — EvtThread 정지 → io_work_queue push 없음
        //   4) io_worker_thread Stop  — io_work_queue 안 남은 cv::Mat cleanup 후 정지
        this->inference_thread_.Stop();
        this->response_thread_.Stop();
        this->event_thread_.Stop();
        this->io_worker_thread_.Stop();
        return true;
    }

    static const size_t ConvertTrackingBoxesToMetaData( const vector<InferObject>& infer_objects, string& metadata_string )
    {
        // get time
        auto        now   = std::chrono::system_clock::now();
        auto        ms    = std::chrono::duration_cast<std::chrono::milliseconds>( now.time_since_epoch() ) % 1000;
        std::time_t now_c = std::chrono::system_clock::to_time_t( now );

        struct tm tstruct {};
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
            "</tt:MetadataStream>";

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
            metadata_string += objectxml;
        }
        metadata_string += postXml;

        return metadata_string.size() + 1;
    }

    void RtspDetectorUnit::SendDetectResultToMetaData( const vector<InferObject>& detect_results )
    {
        // Phase 2: ONVIF metadata 송신 — GstRtspProxyServer + OnvifMetadataPayloader 경로.
        //   1) detect_results → ONVIF Streaming v25.12 XML (ConvertTrackingBoxesToMetaData)
        //   2) RtspHandler 의 per-cam payloader 가 RTP 헤더 작성 후 pay1 appsrc 로 push.
        //   클라이언트 미접속 시 payloader 가 미생성 — SendOnvifMetadata 가 false 반환 (정상).
        if( !rtsp_handler_ ) return;

        std::string metadata_xml;
        ConvertTrackingBoxesToMetaData( detect_results, metadata_xml );
        if( metadata_xml.empty() ) return;

        rtsp_handler_->SendOnvifMetadata( id_, metadata_xml );
    }

    std::string RtspDetectorUnit::GetFrameImageCurrentProxyRootPath( void ) const
    {
        // P61: DefineDefault 의 단일 진실 사용.
        return std::string { DefineDefault::FULL_FRAME_IMAGE_SAVE_ROOT_PATH };
    }

    std::optional<std::string> RtspDetectorUnit::MakeImageSavePath( const std::string& root_path ) const
    {
        struct tm tstruct {};
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
        const std::shared_ptr<Abnormal::Schedule>& sch,
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
        const std::string& ImagePath = image_path;
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

                // B2 — ResponseThread 가 tracker 분기 시 사용. cam thread 초기화 시 한 번 set, read-only.
                this->class_id_person_ = class_id_person;
                this->class_id_car_    = class_id_car;

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
        // Warm up — Phase 1 (GStreamer): SettingManager 에서 cam URL/auth 를 가져와
        // 자체 GstRtspClient 를 생성하고 avframe_q_ 를 producer 로 등록.
        // ownership 은 rtsp_handler_ 가 보유, 여기서는 weak raw ptr 만 보존.
        auto& running = this->inference_thread_.GetRunningFlag();

        {
            auto setting_manager  = MGEN::GetSettingManager();
            auto cam_settings_mgr = setting_manager ? setting_manager->GetCameraSettingsManager() : nullptr;
            std::optional<CameraSettingData> cam_setting_opt;
            if( cam_settings_mgr ) {
                cam_setting_opt = cam_settings_mgr->GetSetting( id_ );
            }

            if( cam_setting_opt.has_value() == false ) {
                MLOG_ERROR("CAM[%d]::InferenceThread: SettingManager 에 카메라 설정 없음. Unit 종료.", id_ );
                return;
            }

            GstRtspClient::Config cfg;
            cfg.cam_id          = id_;
            cfg.rtsp_url        = cam_setting_opt->url;
            cfg.user_id         = cam_setting_opt->access_id;
            cfg.user_pw         = cam_setting_opt->access_pw;
            // bugprone-misplaced-widening-cast: 2 * detect_fps_limit_ (int*int) 후 cast → cast 먼저 후 곱셈.
            cfg.queue_max_size  = 2 * static_cast<size_t>( detect_fps_limit_ );
            cfg.fps_limit       = detect_fps_limit_;  // happytimesoft 호환 frame skip (큐 size>0 drop + interval drop)
            cfg.enable_raw_passthrough = true;        // Phase 2: raw H.264 byte-stream → RtspHandler → proxy server video forward

            auto gst_client = std::make_unique<GstRtspClient>( cfg, avframe_q_ );

            // Phase 2: receiver 의 raw H.264 byte-stream 을 proxy server 로 forward.
            //   shared_ptr capture — handler lifetime 이 callback 보다 길도록 보장.
            auto handler_sp       = rtsp_handler_;
            const int cam_id_capt = id_;
            gst_client->SetRawPacketCallback(
                [handler_sp, cam_id_capt]( const uint8_t* data, size_t size ) {
                    if( handler_sp ) handler_sp->ForwardVideoRtp( cam_id_capt, data, size );
                } );

            if( !gst_client->Start() ) {
                MLOG_ERROR("CAM[%d]::InferenceThread: GstRtspClient Start 실패", id_ );
                return;
            }

            proxy_ptr_ = gst_client.get();  // weak ref — RtspHandler 가 ownership
            rtsp_handler_->RegisterClient( id_, std::move( gst_client ) );
            MLOG_INFO("CAM[%d]::InferenceThread: GstRtspClient registered (url=%s)", id_, cfg.rtsp_url.c_str() );
        }

        // exit early
        if( running.load() == false )
            return;

        // queue 는 GstRtspClient 의 ctor 에서 이미 producer 로 연결됨.
        // happytimesoft 의 setDecodedFrameSafeQueue 호출은 불필요.

        // const
        const auto dequeue_timeout                = 100ms;
        const int  inference_wait_ms              = 5000;  // 5sec
        const auto long_timelapse_log_interval = 10min; // 10min

        // set thread internal timer
        EventTime avframe_current_recv_time     = std::chrono::system_clock::now();
        EventTime last_interval_log_print_time  = std::chrono::system_clock::now();

        // reset members
        this->consecutive_mismatch_count_ = 0;

#ifdef DEBUG_MODE
        // InferenceThread (cam side) stage profile — 모든 단계 us 단위 평균, 100 cycle 마다 1줄 로그.
        // Release 빌드에선 본 블록 전체가 preprocessor 제거 → 0 runtime cost.
        struct InfProf {
            uint64_t dq_us = 0;             // avframe_q dequeue
            uint64_t pre_us = 0;            // preprocess (sws_scale 포함)
            uint64_t req_us = 0;            // RequestAsync per engine
            uint64_t push_us = 0;           // inflight_q->enqueue_move
            uint64_t avframe_size_sum = 0;  // avframe_q size 누적 (dq 직후 잔여)
            uint64_t avframe_size_max = 0;
            uint64_t inflight_size_sum = 0; // inflight_q size 누적 (push 직전 — 누적 leak 검증용)
            uint64_t inflight_size_max = 0;
            uint64_t count = 0;
        };
        InfProf inf_prof;
        auto inf_us = []( const std::chrono::steady_clock::time_point& a,
                          const std::chrono::steady_clock::time_point& b ) -> uint64_t {
            return static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>( b - a ).count() );
        };
#endif

        // Main
        while( running.load() == true )
        {
            DBG_PROF( const auto t_inf_top = std::chrono::steady_clock::now(); )
            DBG_PROF( std::chrono::steady_clock::time_point t_after_dq, t_after_pre, t_after_req, t_after_push; )
            DBG_PROF( bool t_dq_set = false, t_pre_set = false, t_req_set = false, t_push_set = false; )
            // Read AVFrame from GStreamer pipeline (avframe_q_ 의 producer 는 GstRtspClient)
            // dequeue_wait_for 는 종료/타임아웃 시 std::nullopt 반환 (throw 없음)
            std::optional<std::shared_ptr<AVFrame>> opt_frame = avframe_q_->dequeue_wait_for( dequeue_timeout );
            DBG_PROF( t_after_dq = std::chrono::steady_clock::now(); )
            DBG_PROF( t_dq_set = true; )
            DBG_PROF( const size_t cyc_avframe_size = avframe_q_ ? avframe_q_->size() : 0; )  // dq 직후 잔여 (큐 누적 검증)

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
#ifdef DEBUG_MODE
                // 진단 (debug/gst-rtsp-stale-trace 2026-05-20) — 매 timeout 시 last_frame_age gauge update.
                //   stuck 시 InferenceThread 의 100-cycle 영역 도달 안 함 → 이 위치에서만 gauge update.
                //   monotonic increase 면 cam stream stuck 의 외부 signal.
                if( proxy_ptr_ ) {
                    const int64_t last_ns = proxy_ptr_->GetLastFrameNs();
                    if( last_ns > 0 ) {
                        const int64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                  std::chrono::steady_clock::now().time_since_epoch() ).count();
                        const double age_sec = static_cast<double>( now_ns - last_ns ) / 1e9;
                        MGEN::MetricsRegistry::Instance().SetGauge(
                            "detectbase_gst_rtsp_last_frame_age_sec",
                            { { "cam_id", std::to_string( id_ ) } },
                            age_sec );
                    }
                }
#endif
                continue;
            }
            else
            {
                // check frame recv time
                avframe_current_recv_time = std::chrono::system_clock::now();
            }

            // get AVFrame ptr — std::move 로 atomic ref-count op 회피 (TSan happens-before 추적 한계).
            std::shared_ptr<AVFrame> frame = std::move( *opt_frame );

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

            // Schedule update
            if( this->is_schedule_updated_.load() || need_reset_schedule_n_tracker_cuz_resize )
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
            DBG_PROF( t_after_pre = std::chrono::steady_clock::now(); )
            DBG_PROF( t_pre_set = true; )

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
            DBG_PROF( t_after_req = std::chrono::steady_clock::now(); )
            DBG_PROF( t_req_set = true; )

            // B2 async pipeline — RespondAsync + tracking + metadata + event/emit 를 ResponseThread 에 위임.
            //   cam thread 의 cycle 에서 NPU resp(22ms) + post(12ms) = 34ms 빠짐. dfps +52% 기대.
            //   InflightItem 안에 ResponseThread 가 필요한 데이터 self-contained 으로 복사.
            InflightItem item;
            item.frame             = frame;
            item.correlation_id    = current_correlation_id;
            item.request_time      = std::chrono::steady_clock::now();
            item.inference_wait_ms = inference_wait_ms;
            item.engines.reserve( requested_engines.size() );
            for( auto* engine : requested_engines ) {
                EngineInflight ei;
                ei.subscribe_id     = engine->subscribe_id;
                ei.input_width      = engine->input_width;
                ei.input_height     = engine->input_height;
                ei.target_class_ids = engine->target_class_ids;
                ei.magic_name       = engine->magic_name;
                if( auto* ctx = context_manager.GetContext( engine->input_width, engine->input_height, frame ) ) {
                    ei.inference_style = ctx->inference_style;
                    ei.original_style  = ctx->original_style;
                }
                item.engines.push_back( std::move( ei ) );
            }

            DBG_PROF( size_t cyc_inflight_size = 0; )
            if( inflight_q_ ) {
                DBG_PROF( cyc_inflight_size = inflight_q_->size(); )  // push 직전 size (누적 leak 검증)
                // SafeQueue::enqueue_move 는 max_size 도달 시 oldest drop. response thread 가 느리면 자연 backpressure.
                inflight_q_->enqueue_move( std::move( item ) );
            }
            DBG_PROF( t_after_push = std::chrono::steady_clock::now(); )
            DBG_PROF( t_push_set = true; )

#ifdef DEBUG_MODE
            // InferenceThread stage profile — 정상 완주 cycle 만 누적, 100 cycle 마다 1줄 로그.
            if( t_dq_set && t_pre_set && t_req_set && t_push_set ) {
                inf_prof.dq_us             += inf_us( t_inf_top, t_after_dq );
                inf_prof.pre_us            += inf_us( t_after_dq, t_after_pre );
                inf_prof.req_us            += inf_us( t_after_pre, t_after_req );
                inf_prof.push_us           += inf_us( t_after_req, t_after_push );
                inf_prof.avframe_size_sum  += cyc_avframe_size;
                if( cyc_avframe_size > inf_prof.avframe_size_max ) inf_prof.avframe_size_max = cyc_avframe_size;
                inf_prof.inflight_size_sum += cyc_inflight_size;
                if( cyc_inflight_size > inf_prof.inflight_size_max ) inf_prof.inflight_size_max = cyc_inflight_size;
                inf_prof.count             += 1;
                if( inf_prof.count >= 100 ) {
                    // GStreamer pipeline 내부 buffer + camera frame interval 실측 — leak hunt
                    const uint32_t gst_buf  = proxy_ptr_ ? proxy_ptr_->GetAppSinkQueuedBuffers() : 0;
                    const uint64_t cam_int  = proxy_ptr_ ? proxy_ptr_->GetFrameIntervalAvgUs() : 0;
                    MLOG_INFO("CAM[%d] INF-thread (avg over %llu cycles, us): dq=%llu pre=%llu req=%llu push=%llu total=%llu | avframe_q avg=%llu max=%llu | inflight_q avg=%llu max=%llu | gst_appsink_buf=%u camera_interval_us=%llu",
                        id_,
                        static_cast<unsigned long long>( inf_prof.count ),
                        static_cast<unsigned long long>( inf_prof.dq_us / inf_prof.count ),
                        static_cast<unsigned long long>( inf_prof.pre_us / inf_prof.count ),
                        static_cast<unsigned long long>( inf_prof.req_us / inf_prof.count ),
                        static_cast<unsigned long long>( inf_prof.push_us / inf_prof.count ),
                        static_cast<unsigned long long>( ( inf_prof.dq_us + inf_prof.pre_us + inf_prof.req_us + inf_prof.push_us ) / inf_prof.count ),
                        static_cast<unsigned long long>( inf_prof.avframe_size_sum / inf_prof.count ),
                        static_cast<unsigned long long>( inf_prof.avframe_size_max ),
                        static_cast<unsigned long long>( inf_prof.inflight_size_sum / inf_prof.count ),
                        static_cast<unsigned long long>( inf_prof.inflight_size_max ),
                        gst_buf,
                        static_cast<unsigned long long>( cam_int ) );
                    inf_prof = InfProf{};
                }
            }
#endif

        } // while running true

    }

    void RtspDetectorUnit::InferenceThreadCloser( void )
    {
        for( const auto& subs_id : subscribe_ids_ ){
            load_balancer_->Unsubscribe( subs_id );
        }

        avframe_q_->terminate();
        avframe_q_->clear_without_action();

        // B2 — inflight_q 도 terminate. ResponseThread 가 곧 종료될 수 있도록 신호.
        if( inflight_q_ ) {
            inflight_q_->terminate();
        }
    }

    // ------------------------------------------------------------------------------------
    //  B2 async pipeline — ResponseThread:
    //    InferenceThread 가 RequestAsync 후 inflight_q_ 에 push.
    //    여기서 dequeue → RespondAsync (NPU 응답 대기) → tracking → ONVIF metadata → schedule check → emit + io_work_queue.
    //
    //  cam thread cycle 에서 NPU resp + post 가 빠짐 → dfps +50% 기대 (이론 ~108).
    //  shutdown 순서: inference_thread → inflight_q->terminate → response_thread (race-free).
    // ------------------------------------------------------------------------------------
    void RtspDetectorUnit::ResponseThreadRunner( void )
    {
        using namespace std::chrono_literals;

        // P53: 이 thread 의 모든 MLOG 에 정적 correlation_id 부여.
        MGEN::CorrelationContext::Set( "sys-response-" + std::to_string( this->id_ ) );

        auto& running = this->response_thread_.GetRunningFlag();

        // Response thread 자체 origin_ctx — cam thread 의 origin_ctx 와 분리.
        // 이벤트 발생 시 1080p frame 의 cv::Mat 변환 + cv::imwrite 위해 io_work_queue 에 넘김.
        FrameFormattingContext resp_origin_ctx;

#ifdef DEBUG_MODE
        // ResponseThread stage profile — 모든 단계 us 단위 평균.
        // Release 빌드에선 본 영역 (struct + lambda + jemalloc/proc 파싱) 전부 preprocessor 제거.
        struct RspProf {
            uint64_t ifq_us  = 0;   // inflight_q dequeue wait
            uint64_t resp_us = 0;   // RespondAsync + bbox merge (NPU 왕복)
            uint64_t trk_us  = 0;   // tracking
            uint64_t meta_us = 0;   // SendDetectResultToMetaData
            uint64_t ev_us   = 0;   // schedule + emit + io_work_queue + grpc (event 측 post)
            uint64_t io_size_sum   = 0;  // io_work_queue size 누적 (push 직전 — cv::Mat 누적 감시)
            uint64_t io_size_max   = 0;
            uint64_t trk_cnt_sum   = 0;  // 이 cycle 의 tracking output 개수
            uint64_t trk_cnt_max   = 0;
            uint64_t lb_resp_size_sum = 0;  // LoadBalancer infer_respond_receiver size (단일 응답 큐)
            uint64_t lb_resp_size_max = 0;
            uint64_t lb_input_size_sum = 0; // LoadBalancer 모든 handler input queue 합산
            uint64_t lb_input_size_max = 0;
            uint64_t sched_count = 0;       // scheduler_ size (event schedule 개수)
            uint64_t trk_obj_count = 0;     // trackers_ size (class id 별 tracker 수)
            uint64_t sio_emit_count = 0;    // 누적 sio emit 호출 (event 마다 ++)
            uint64_t grpc_send_count = 0;   // 누적 GRPC send 호출
            // leak hunt v4 추가 — 내 코드 안 leak 후보 식별.
            uint64_t avf_alive_sum = 0;     // GstRtspReceiver process-wide AVFrame alive count 누적
            uint64_t avf_alive_max = 0;
            uint64_t reset_cnt_total = 0;   // 이 cam 의 ResetSourceOnly 호출 누적 (proxy_ptr_)
            uint64_t kalman_alive_sum = 0;  // trackers_ 의 GetAliveKalmanCount() 합산 누적
            uint64_t kalman_alive_max = 0;
            uint64_t kalman_created_total = 0;  // trackers_ 의 GetCreatedKalmanCount() 합산 (snapshot)
            uint64_t lb_pending_sum = 0;    // EngineLoadBalancer ReplyDispatcher entry 누적
            uint64_t lb_pending_max = 0;
            uint64_t ev_path_count = 0;     // event path (entire_track_results.empty() == false) 진입 누적
            uint64_t ev_json_kb_sum = 0;    // event_msg_json.dump().size() / 1024 누적
            uint64_t ev_push_count = 0;     // io_work_queue->enqueue_move 호출 누적 (event path)
            uint64_t anon_mb_sample = 0;    // /proc/self/maps 의 anonymous mapping 합산 (MB, 측정 시점 sample)
            // SafeQueue drop_oldest 누적 카운터 (v5 추가).
            uint64_t avframe_drop_total  = 0;
            uint64_t inflight_drop_total = 0;
            uint64_t event_drop_total    = 0;
            uint64_t io_drop_total       = 0;
            // v7 — resp_us 분해.
            uint64_t resp_wait_us_sum    = 0;   // RespondAsync wait_and_get (NPU 결과 대기)
            uint64_t resp_wait_us_max    = 0;
            uint64_t resp_merge_us_sum   = 0;   // bbox convert + merge for loop
            uint64_t resp_merge_us_max   = 0;
            uint64_t resp_obj_count_sum  = 0;   // infer_objects 합산
            uint64_t resp_obj_count_max  = 0;
            uint64_t event_q_size_sum    = 0;   // event_q push 직전 size
            uint64_t event_q_size_max    = 0;
            uint64_t count   = 0;
        };
        RspProf rsp_prof;

        // leak hunt v4 helper — /proc/self/maps 의 anonymous mapping 합산 (KB).
        // anonymous = 마지막 column (pathname) 가 비었거나 "[heap]" 또는 "[anon..." 또는 "[stack]" 등 [ 로 시작.
        auto read_anon_mapping_kb = []() -> uint64_t {
            FILE* fp = std::fopen( "/proc/self/maps", "r" );
            if( !fp ) return 0;
            uint64_t total_kb = 0;
            char line[ 512 ];
            while( std::fgets( line, sizeof( line ), fp ) ) {
                uint64_t start = 0, end = 0;
                char perms[ 8 ] = {0};
                int matched = std::sscanf( line, "%lx-%lx %7s", &start, &end, perms );
                if( matched < 3 ) continue;
                // pathname 위치 (6번째 column). 짧으면 anonymous.
                int spaces = 0;
                const char* p = line;
                while( *p && spaces < 5 ) {
                    if( *p == ' ' ) spaces++;
                    p++;
                }
                while( *p == ' ' ) p++;
                bool is_anon = ( *p == '\0' || *p == '\n' || *p == '[' );
                if( is_anon ) total_kb += ( end - start ) / 1024;
            }
            std::fclose( fp );
            return total_kb;
        };

        // jemalloc stats — dlsym 으로 mallctl symbol 동적 찾기 (jemalloc 가 LD_PRELOAD 됐을 때 작동).
        typedef int (*mallctl_t)(const char*, void*, size_t*, void*, size_t);
        auto mallctl_fn = reinterpret_cast<mallctl_t>( dlsym( RTLD_DEFAULT, "mallctl" ) );
        auto rsp_us = []( const std::chrono::steady_clock::time_point& a,
                          const std::chrono::steady_clock::time_point& b ) -> uint64_t {
            return static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>( b - a ).count() );
        };
#endif

        while( running.load() == true )
        {
            // inflight_q 가 nullptr 인 race 방어. Init 가 끝난 후에야 set 되므로 일반적으로 nullptr 아님.
            if( !inflight_q_ ) {
                std::this_thread::sleep_for( 10ms );
                continue;
            }

            DBG_PROF( const auto t_rsp_top = std::chrono::steady_clock::now(); )
            DBG_PROF( std::chrono::steady_clock::time_point t_after_ifq, t_after_resp, t_after_trk, t_after_meta, t_after_ev; )
            DBG_PROF( bool t_ifq_set = false, t_resp_set = false, t_trk_set = false, t_meta_set = false, t_ev_set = false; )
            DBG_PROF( size_t cyc_io_size = 0; )
            DBG_PROF( size_t cyc_trk_cnt = 0; )

            auto opt_item = inflight_q_->dequeue_wait_for( 100ms );
            DBG_PROF( t_after_ifq = std::chrono::steady_clock::now(); )
            DBG_PROF( t_ifq_set = true; )

            if( !opt_item.has_value() ) {
                if( inflight_q_->is_terminated() ) {
                    break;
                }
                continue;
            }

            InflightItem item = std::move( *opt_item );
            // shared_ptr copy → atomic ref-count add (TSan happens-before 추적 한계로 race report).
            // std::move 로 ownership 이양 → atomic op 없음. item.frame 은 이후 unused.
            auto frame = std::move( item.frame );
            if( !frame ) continue;

            // -----------------------------------------------------------------
            // 1. RespondAsync + bbox convert + merge
            //    v7 — resp_us 분해: wait (NPU 결과 대기) vs merge (bbox convert loop).
            // -----------------------------------------------------------------
            std::vector<InferObject> merged_results;
            DBG_PROF( uint64_t cyc_resp_wait_us   = 0; )
            DBG_PROF( uint64_t cyc_resp_merge_us  = 0; )
            DBG_PROF( size_t   cyc_resp_obj_count = 0; )
            for( const auto& engine_inflight : item.engines )
            {
                auto now              = std::chrono::steady_clock::now();
                auto elapsed_ms       = std::chrono::duration_cast<std::chrono::milliseconds>( now - item.request_time ).count();
                int  remaining_budget = item.inference_wait_ms - static_cast<int>( elapsed_ms );

                if( remaining_budget <= 0 ) {
                    MLOG_WARN("CAM[%d] Engine %s skipped (Total budget %dms exhausted)",
                        id_, engine_inflight.magic_name.c_str(), item.inference_wait_ms );
                    continue;
                }

                DBG_PROF( const auto t_wait_start = std::chrono::steady_clock::now(); )
                auto result_opt = load_balancer_->RespondAsync( engine_inflight.subscribe_id, remaining_budget );
                DBG_PROF( const auto t_wait_end = std::chrono::steady_clock::now(); )
                DBG_PROF( cyc_resp_wait_us += rsp_us( t_wait_start, t_wait_end ); )

                if( result_opt.has_value() )
                {
                    // 방어 카운터: round-robin handler 의 NPU 처리시간 variance 때문에 cam_result_qs_ 의 응답
                    // 순서와 inflight_q 의 frame 순서가 어긋날 가능성. 발생 빈도 측정용 (실측 후 fix 결정).
                    // NPU rknn_run 시간은 거의 일정 (~22ms ± few ms) 이라 실제 빈도 매우 낮을 것으로 예상.
                    if( result_opt->meta_data.correlation_id != item.correlation_id ) {
                        MGEN::MetricsRegistry::Instance().IncrementCounter(
                            "detectbase_correlation_mismatch_total",
                            { { "cam_id", std::to_string( id_ ) } } );
                        MLOG_WARN( "CAM[%d] correlation_id mismatch — expected %lu, got %lu (engine=%s)",
                            id_,
                            static_cast<unsigned long>( item.correlation_id ),
                            static_cast<unsigned long>( result_opt->meta_data.correlation_id ),
                            engine_inflight.magic_name.c_str() );
                    }

                    load_balancer_->AddInferCount( engine_inflight.subscribe_id );
                    DBG_PROF( cyc_resp_obj_count += result_opt->infer_objects.size(); )

                    for( const auto& obj : result_opt->infer_objects )
                    {
                        bool target_found = false;
                        for( auto const& [ name, id ] : engine_inflight.target_class_ids )
                        {
                            if( id == obj.class_id ) { target_found = true; break; }
                        }
                        if( target_found )
                        {
                            InferObject target = obj;
                            ConvertInferObjectCoordinate( target, engine_inflight.inference_style, engine_inflight.original_style );
                            merged_results.push_back( target );
                        }
                    }
                }
                else {
                    MLOG_WARN("CAM[%d] Engine %s timeout (Waited max %d ms)",
                        id_, engine_inflight.magic_name.c_str(), remaining_budget );
                }
                DBG_PROF( const auto t_merge_end = std::chrono::steady_clock::now(); )
                DBG_PROF( cyc_resp_merge_us += rsp_us( t_wait_end, t_merge_end ); )
            }
            DBG_PROF( t_after_resp = std::chrono::steady_clock::now(); )
            DBG_PROF( t_resp_set = true; )

#ifdef DEBUG_MODE
            // v7 accumulate
            rsp_prof.resp_wait_us_sum  += cyc_resp_wait_us;
            if( cyc_resp_wait_us > rsp_prof.resp_wait_us_max ) rsp_prof.resp_wait_us_max = cyc_resp_wait_us;
            rsp_prof.resp_merge_us_sum += cyc_resp_merge_us;
            if( cyc_resp_merge_us > rsp_prof.resp_merge_us_max ) rsp_prof.resp_merge_us_max = cyc_resp_merge_us;
            rsp_prof.resp_obj_count_sum += cyc_resp_obj_count;
            if( cyc_resp_obj_count > rsp_prof.resp_obj_count_max ) rsp_prof.resp_obj_count_max = cyc_resp_obj_count;
#endif

            // -----------------------------------------------------------------
            // 2. Tracking
            // -----------------------------------------------------------------
            std::vector<InferObject> entire_track_results;
            std::map<InferClassID, std::vector<InferObject>> classified_objects;

            for( const auto& obj : merged_results ){
                classified_objects[ obj.class_id ].push_back( obj );
            }

            for( auto& [ class_id, objects ] : classified_objects )
            {
                const bool need_tracking
                    = ( class_id == class_id_person_ ) || ( class_id == class_id_car_ );

                if( need_tracking )
                {
                    if( trackers_.find( class_id ) == trackers_.end() )
                    {
                        ImageExpressStyle tracker_style( frame->width, frame->height );
                        trackers_[ class_id ]     = std::make_unique<SORTTracker>( tracker_style, tracker_style );
                        tracker_seqs_[ class_id ] = 1;
                    }
                    auto& seq = tracker_seqs_[ class_id ];
                    auto  res = trackers_[ class_id ]->TrackObjects( objects, static_cast<int>( seq++ ) );
                    if( res ) {
                        entire_track_results.insert( entire_track_results.end(), res->begin(), res->end() );
                    }
                }
                else
                {
                    entire_track_results.insert( entire_track_results.end(), objects.begin(), objects.end() );
                }
            }

            DBG_PROF( t_after_trk = std::chrono::steady_clock::now(); )
            DBG_PROF( t_trk_set = true; )

            DBG_PROF( cyc_trk_cnt = entire_track_results.size(); )  // 이 cycle 의 tracking output 개수 (운영 추세)

            SendDetectResultToMetaData( entire_track_results );
            DBG_PROF( t_after_meta = std::chrono::steady_clock::now(); )
            DBG_PROF( t_meta_set = true; )

            if( entire_track_results.empty() ) {
                DBG_PROF( t_after_ev = std::chrono::steady_clock::now(); )
                DBG_PROF( t_ev_set = true; )
#ifdef DEBUG_MODE
                if( t_ifq_set && t_resp_set && t_trk_set && t_meta_set && t_ev_set ) {
                    rsp_prof.ifq_us           += rsp_us( t_rsp_top, t_after_ifq );
                    rsp_prof.resp_us          += rsp_us( t_after_ifq, t_after_resp );
                    rsp_prof.trk_us           += rsp_us( t_after_resp, t_after_trk );
                    rsp_prof.meta_us          += rsp_us( t_after_trk, t_after_meta );
                    rsp_prof.ev_us            += rsp_us( t_after_meta, t_after_ev );
                    rsp_prof.io_size_sum      += cyc_io_size;
                    if( cyc_io_size > rsp_prof.io_size_max ) rsp_prof.io_size_max = cyc_io_size;
                    rsp_prof.trk_cnt_sum      += cyc_trk_cnt;
                    if( cyc_trk_cnt > rsp_prof.trk_cnt_max ) rsp_prof.trk_cnt_max = cyc_trk_cnt;
                    // LoadBalancer 응답 큐 + input queue (모든 handler 합산) — leak hunt
                    const size_t lb_resp  = load_balancer_ ? load_balancer_->GetRespondReceiverSize() : 0;
                    const size_t lb_input = load_balancer_ ? load_balancer_->GetTotalInputQueueSize() : 0;
                    rsp_prof.lb_resp_size_sum += lb_resp;
                    if( lb_resp > rsp_prof.lb_resp_size_max ) rsp_prof.lb_resp_size_max = lb_resp;
                    rsp_prof.lb_input_size_sum += lb_input;
                    if( lb_input > rsp_prof.lb_input_size_max ) rsp_prof.lb_input_size_max = lb_input;
                    rsp_prof.sched_count     = scheduler_.size();
                    rsp_prof.trk_obj_count   = trackers_.size();
                    // leak hunt v4 — 매 cycle sample (cheap)
                    {
                        const uint64_t avf_alive = MGEN::GstRtspReceiver::GetAvFrameAliveCount();
                        rsp_prof.avf_alive_sum += avf_alive;
                        if( avf_alive > rsp_prof.avf_alive_max ) rsp_prof.avf_alive_max = avf_alive;
                        uint64_t kalman_alive = 0, kalman_created = 0;
                        for( const auto& [cid, trk] : trackers_ ) {
                            (void) cid;
                            if( trk ) { kalman_alive += trk->GetAliveKalmanCount(); kalman_created += trk->GetCreatedKalmanCount(); }
                        }
                        rsp_prof.kalman_alive_sum += kalman_alive;
                        if( kalman_alive > rsp_prof.kalman_alive_max ) rsp_prof.kalman_alive_max = kalman_alive;
                        rsp_prof.kalman_created_total = kalman_created;
                        // B4 — cam-별 result queue 의 합산 size (이전 GetReplyDispatcherEntrySize 는 deprecated, 항상 0).
                        const uint64_t lb_pending = load_balancer_ ? load_balancer_->GetCamResultQTotalSize() : 0;
                        rsp_prof.lb_pending_sum += lb_pending;
                        if( lb_pending > rsp_prof.lb_pending_max ) rsp_prof.lb_pending_max = lb_pending;
                        rsp_prof.reset_cnt_total = proxy_ptr_ ? proxy_ptr_->GetResetSourceCount() : 0;
                        // v5 — SafeQueue drop counter snapshot (모든 큐).
                        rsp_prof.avframe_drop_total  = avframe_q_     ? avframe_q_->GetDropCount()     : 0;
                        rsp_prof.inflight_drop_total = inflight_q_    ? inflight_q_->GetDropCount()    : 0;
                        rsp_prof.event_drop_total    = event_q_       ? event_q_->GetDropCount()       : 0;
                        rsp_prof.io_drop_total       = io_work_queue_ ? io_work_queue_->GetDropCount() : 0;
                    }
                    rsp_prof.count           += 1;
                    if( rsp_prof.count >= 100 ) {
                        // jemalloc stats (LD_PRELOAD 활성 시)
                        uint64_t je_alloc = 0, je_active = 0, je_resident = 0, je_mapped = 0;
                        if( mallctl_fn ) {
                            uint64_t epoch = 1; size_t sz = sizeof(epoch);
                            mallctl_fn( "epoch", &epoch, &sz, &epoch, sz );
                            sz = sizeof(je_alloc);    mallctl_fn( "stats.allocated", &je_alloc,    &sz, nullptr, 0 );
                            sz = sizeof(je_active);   mallctl_fn( "stats.active",    &je_active,   &sz, nullptr, 0 );
                            sz = sizeof(je_resident); mallctl_fn( "stats.resident",  &je_resident, &sz, nullptr, 0 );
                            sz = sizeof(je_mapped);   mallctl_fn( "stats.mapped",    &je_mapped,   &sz, nullptr, 0 );
                        }
                        // 100 cycle 마다 한 번만 anon mapping 측정 (expensive — /proc/self/maps 전체 파싱).
                        rsp_prof.anon_mb_sample = read_anon_mapping_kb() / 1024;
                        MLOG_INFO("CAM[%d] RSP-thread (avg over %llu cycles, us): ifq=%llu resp=%llu trk=%llu meta=%llu ev=%llu total=%llu | resp_wait avg=%llu max=%llu | resp_merge avg=%llu max=%llu | resp_obj avg=%llu max=%llu | event_q_push avg=%llu max=%llu | io_q avg=%llu max=%llu | trk_cnt avg=%llu max=%llu | lb_resp avg=%llu max=%llu | lb_input avg=%llu max=%llu | sched=%llu trk_map=%llu | sio_emit=%llu grpc=%llu | jem_alloc=%lluKB active=%lluKB resident=%lluKB mapped=%lluKB | avf_alive avg=%llu max=%llu | reset_cnt=%llu | kalman alive avg=%llu max=%llu created=%llu | lb_pending avg=%llu max=%llu | ev_path=%llu ev_json_kb=%llu ev_push=%llu | anon=%lluMB | q_drop avf=%llu inf=%llu ev=%llu io=%llu",
                            id_,
                            static_cast<unsigned long long>( rsp_prof.count ),
                            static_cast<unsigned long long>( rsp_prof.ifq_us  / rsp_prof.count ),
                            static_cast<unsigned long long>( rsp_prof.resp_us / rsp_prof.count ),
                            static_cast<unsigned long long>( rsp_prof.trk_us  / rsp_prof.count ),
                            static_cast<unsigned long long>( rsp_prof.meta_us / rsp_prof.count ),
                            static_cast<unsigned long long>( rsp_prof.ev_us   / rsp_prof.count ),
                            static_cast<unsigned long long>( ( rsp_prof.ifq_us + rsp_prof.resp_us + rsp_prof.trk_us + rsp_prof.meta_us + rsp_prof.ev_us ) / rsp_prof.count ),
                            static_cast<unsigned long long>( rsp_prof.resp_wait_us_sum  / rsp_prof.count ),
                            static_cast<unsigned long long>( rsp_prof.resp_wait_us_max ),
                            static_cast<unsigned long long>( rsp_prof.resp_merge_us_sum / rsp_prof.count ),
                            static_cast<unsigned long long>( rsp_prof.resp_merge_us_max ),
                            static_cast<unsigned long long>( rsp_prof.resp_obj_count_sum / rsp_prof.count ),
                            static_cast<unsigned long long>( rsp_prof.resp_obj_count_max ),
                            static_cast<unsigned long long>( rsp_prof.event_q_size_sum / rsp_prof.count ),
                            static_cast<unsigned long long>( rsp_prof.event_q_size_max ),
                            static_cast<unsigned long long>( rsp_prof.io_size_sum / rsp_prof.count ),
                            static_cast<unsigned long long>( rsp_prof.io_size_max ),
                            static_cast<unsigned long long>( rsp_prof.trk_cnt_sum / rsp_prof.count ),
                            static_cast<unsigned long long>( rsp_prof.trk_cnt_max ),
                            static_cast<unsigned long long>( rsp_prof.lb_resp_size_sum / rsp_prof.count ),
                            static_cast<unsigned long long>( rsp_prof.lb_resp_size_max ),
                            static_cast<unsigned long long>( rsp_prof.lb_input_size_sum / rsp_prof.count ),
                            static_cast<unsigned long long>( rsp_prof.lb_input_size_max ),
                            static_cast<unsigned long long>( rsp_prof.sched_count ),
                            static_cast<unsigned long long>( rsp_prof.trk_obj_count ),
                            static_cast<unsigned long long>( rsp_prof.sio_emit_count ),
                            static_cast<unsigned long long>( rsp_prof.grpc_send_count ),
                            static_cast<unsigned long long>( je_alloc / 1024 ),
                            static_cast<unsigned long long>( je_active / 1024 ),
                            static_cast<unsigned long long>( je_resident / 1024 ),
                            static_cast<unsigned long long>( je_mapped / 1024 ),
                            static_cast<unsigned long long>( rsp_prof.avf_alive_sum / rsp_prof.count ),
                            static_cast<unsigned long long>( rsp_prof.avf_alive_max ),
                            static_cast<unsigned long long>( rsp_prof.reset_cnt_total ),
                            static_cast<unsigned long long>( rsp_prof.kalman_alive_sum / rsp_prof.count ),
                            static_cast<unsigned long long>( rsp_prof.kalman_alive_max ),
                            static_cast<unsigned long long>( rsp_prof.kalman_created_total ),
                            static_cast<unsigned long long>( rsp_prof.lb_pending_sum / rsp_prof.count ),
                            static_cast<unsigned long long>( rsp_prof.lb_pending_max ),
                            static_cast<unsigned long long>( rsp_prof.ev_path_count ),
                            static_cast<unsigned long long>( rsp_prof.ev_json_kb_sum ),
                            static_cast<unsigned long long>( rsp_prof.ev_push_count ),
                            static_cast<unsigned long long>( rsp_prof.anon_mb_sample ),
                            static_cast<unsigned long long>( rsp_prof.avframe_drop_total ),
                            static_cast<unsigned long long>( rsp_prof.inflight_drop_total ),
                            static_cast<unsigned long long>( rsp_prof.event_drop_total ),
                            static_cast<unsigned long long>( rsp_prof.io_drop_total ) );
                        rsp_prof = RspProf{};
                    }
                }
#endif
                continue;
            }

            // -----------------------------------------------------------------
            // 3. B3 — track 결과를 event_q push (EvtThread 가 schedule check + sio/grpc + io_work_queue 처리)
            // -----------------------------------------------------------------
            if( event_q_ ) {
                DBG_PROF( const size_t cyc_evq_size = event_q_->size(); )  // v7 — push 직전 event_q size
                DBG_PROF( rsp_prof.event_q_size_sum += cyc_evq_size; )
                DBG_PROF( if( cyc_evq_size > rsp_prof.event_q_size_max ) rsp_prof.event_q_size_max = cyc_evq_size; )

                EventItem ev_item;
                ev_item.frame         = frame;                              // shared_ptr ref +1 (Convert 위해 보존)
                ev_item.track_results = std::move( entire_track_results );  // move (RspThread scope 끝나므로)
                event_q_->enqueue_move( std::move( ev_item ) );             // SafeQueue 가 cap 도달 시 drop_oldest + drop_count_++
                DBG_PROF( rsp_prof.ev_path_count += 1; )
            }

            DBG_PROF( t_after_ev = std::chrono::steady_clock::now(); )
            DBG_PROF( t_ev_set = true; )

#ifdef DEBUG_MODE
            // ResponseThread stage profile flush — event path 도 동일 full format 으로 통합 (v4 leak hunt).
            if( t_ifq_set && t_resp_set && t_trk_set && t_meta_set && t_ev_set ) {
                rsp_prof.ifq_us  += rsp_us( t_rsp_top, t_after_ifq );
                rsp_prof.resp_us += rsp_us( t_after_ifq, t_after_resp );
                rsp_prof.trk_us  += rsp_us( t_after_resp, t_after_trk );
                rsp_prof.meta_us += rsp_us( t_after_trk, t_after_meta );
                rsp_prof.ev_us   += rsp_us( t_after_meta, t_after_ev );
                // event path 의 io_size + LoadBalancer + tracker map 도 sample (empty path 와 동일).
                rsp_prof.io_size_sum      += cyc_io_size;
                if( cyc_io_size > rsp_prof.io_size_max ) rsp_prof.io_size_max = cyc_io_size;
                rsp_prof.trk_cnt_sum      += cyc_trk_cnt;
                if( cyc_trk_cnt > rsp_prof.trk_cnt_max ) rsp_prof.trk_cnt_max = cyc_trk_cnt;
                const size_t lb_resp_ev  = load_balancer_ ? load_balancer_->GetRespondReceiverSize() : 0;
                const size_t lb_input_ev = load_balancer_ ? load_balancer_->GetTotalInputQueueSize() : 0;
                rsp_prof.lb_resp_size_sum += lb_resp_ev;
                if( lb_resp_ev > rsp_prof.lb_resp_size_max ) rsp_prof.lb_resp_size_max = lb_resp_ev;
                rsp_prof.lb_input_size_sum += lb_input_ev;
                if( lb_input_ev > rsp_prof.lb_input_size_max ) rsp_prof.lb_input_size_max = lb_input_ev;
                rsp_prof.sched_count     = scheduler_.size();
                rsp_prof.trk_obj_count   = trackers_.size();
                // leak hunt v4 — 매 cycle sample (cheap)
                {
                    const uint64_t avf_alive = MGEN::GstRtspReceiver::GetAvFrameAliveCount();
                    rsp_prof.avf_alive_sum += avf_alive;
                    if( avf_alive > rsp_prof.avf_alive_max ) rsp_prof.avf_alive_max = avf_alive;
                    uint64_t kalman_alive = 0, kalman_created = 0;
                    for( const auto& [cid, trk] : trackers_ ) {
                        (void) cid;
                        if( trk ) { kalman_alive += trk->GetAliveKalmanCount(); kalman_created += trk->GetCreatedKalmanCount(); }
                    }
                    rsp_prof.kalman_alive_sum += kalman_alive;
                    if( kalman_alive > rsp_prof.kalman_alive_max ) rsp_prof.kalman_alive_max = kalman_alive;
                    rsp_prof.kalman_created_total = kalman_created;
                    const uint64_t lb_pending = load_balancer_ ? load_balancer_->GetReplyDispatcherEntrySize() : 0;
                    rsp_prof.lb_pending_sum += lb_pending;
                    if( lb_pending > rsp_prof.lb_pending_max ) rsp_prof.lb_pending_max = lb_pending;
                    rsp_prof.reset_cnt_total = proxy_ptr_ ? proxy_ptr_->GetResetSourceCount() : 0;
                }
                rsp_prof.count   += 1;
                if( rsp_prof.count >= 100 ) {
                    uint64_t je_alloc = 0, je_active = 0, je_resident = 0, je_mapped = 0;
                    if( mallctl_fn ) {
                        uint64_t epoch = 1; size_t sz = sizeof(epoch);
                        mallctl_fn( "epoch", &epoch, &sz, &epoch, sz );
                        sz = sizeof(je_alloc);    mallctl_fn( "stats.allocated", &je_alloc,    &sz, nullptr, 0 );
                        sz = sizeof(je_active);   mallctl_fn( "stats.active",    &je_active,   &sz, nullptr, 0 );
                        sz = sizeof(je_resident); mallctl_fn( "stats.resident",  &je_resident, &sz, nullptr, 0 );
                        sz = sizeof(je_mapped);   mallctl_fn( "stats.mapped",    &je_mapped,   &sz, nullptr, 0 );
                    }
                    rsp_prof.anon_mb_sample = read_anon_mapping_kb() / 1024;
                    MLOG_INFO("CAM[%d] RSP-thread (avg over %llu cycles, us): ifq=%llu resp=%llu trk=%llu meta=%llu ev=%llu total=%llu | resp_wait avg=%llu max=%llu | resp_merge avg=%llu max=%llu | resp_obj avg=%llu max=%llu | event_q_push avg=%llu max=%llu | io_q avg=%llu max=%llu | trk_cnt avg=%llu max=%llu | lb_resp avg=%llu max=%llu | lb_input avg=%llu max=%llu | sched=%llu trk_map=%llu | sio_emit=%llu grpc=%llu | jem_alloc=%lluKB active=%lluKB resident=%lluKB mapped=%lluKB | avf_alive avg=%llu max=%llu | reset_cnt=%llu | kalman alive avg=%llu max=%llu created=%llu | lb_pending avg=%llu max=%llu | ev_path=%llu ev_json_kb=%llu ev_push=%llu | anon=%lluMB | q_drop avf=%llu inf=%llu ev=%llu io=%llu",
                        id_,
                        static_cast<unsigned long long>( rsp_prof.count ),
                        static_cast<unsigned long long>( rsp_prof.ifq_us  / rsp_prof.count ),
                        static_cast<unsigned long long>( rsp_prof.resp_us / rsp_prof.count ),
                        static_cast<unsigned long long>( rsp_prof.trk_us  / rsp_prof.count ),
                        static_cast<unsigned long long>( rsp_prof.meta_us / rsp_prof.count ),
                        static_cast<unsigned long long>( rsp_prof.ev_us   / rsp_prof.count ),
                        static_cast<unsigned long long>( ( rsp_prof.ifq_us + rsp_prof.resp_us + rsp_prof.trk_us + rsp_prof.meta_us + rsp_prof.ev_us ) / rsp_prof.count ),
                        static_cast<unsigned long long>( rsp_prof.resp_wait_us_sum  / rsp_prof.count ),
                        static_cast<unsigned long long>( rsp_prof.resp_wait_us_max ),
                        static_cast<unsigned long long>( rsp_prof.resp_merge_us_sum / rsp_prof.count ),
                        static_cast<unsigned long long>( rsp_prof.resp_merge_us_max ),
                        static_cast<unsigned long long>( rsp_prof.resp_obj_count_sum / rsp_prof.count ),
                        static_cast<unsigned long long>( rsp_prof.resp_obj_count_max ),
                        static_cast<unsigned long long>( rsp_prof.event_q_size_sum / rsp_prof.count ),
                        static_cast<unsigned long long>( rsp_prof.event_q_size_max ),
                        static_cast<unsigned long long>( rsp_prof.io_size_sum / rsp_prof.count ),
                        static_cast<unsigned long long>( rsp_prof.io_size_max ),
                        static_cast<unsigned long long>( rsp_prof.trk_cnt_sum / rsp_prof.count ),
                        static_cast<unsigned long long>( rsp_prof.trk_cnt_max ),
                        static_cast<unsigned long long>( rsp_prof.lb_resp_size_sum / rsp_prof.count ),
                        static_cast<unsigned long long>( rsp_prof.lb_resp_size_max ),
                        static_cast<unsigned long long>( rsp_prof.lb_input_size_sum / rsp_prof.count ),
                        static_cast<unsigned long long>( rsp_prof.lb_input_size_max ),
                        static_cast<unsigned long long>( rsp_prof.sched_count ),
                        static_cast<unsigned long long>( rsp_prof.trk_obj_count ),
                        static_cast<unsigned long long>( rsp_prof.sio_emit_count ),
                        static_cast<unsigned long long>( rsp_prof.grpc_send_count ),
                        static_cast<unsigned long long>( je_alloc / 1024 ),
                        static_cast<unsigned long long>( je_active / 1024 ),
                        static_cast<unsigned long long>( je_resident / 1024 ),
                        static_cast<unsigned long long>( je_mapped / 1024 ),
                        static_cast<unsigned long long>( rsp_prof.avf_alive_sum / rsp_prof.count ),
                        static_cast<unsigned long long>( rsp_prof.avf_alive_max ),
                        static_cast<unsigned long long>( rsp_prof.reset_cnt_total ),
                        static_cast<unsigned long long>( rsp_prof.kalman_alive_sum / rsp_prof.count ),
                        static_cast<unsigned long long>( rsp_prof.kalman_alive_max ),
                        static_cast<unsigned long long>( rsp_prof.kalman_created_total ),
                        static_cast<unsigned long long>( rsp_prof.lb_pending_sum / rsp_prof.count ),
                        static_cast<unsigned long long>( rsp_prof.lb_pending_max ),
                        static_cast<unsigned long long>( rsp_prof.ev_path_count ),
                        static_cast<unsigned long long>( rsp_prof.ev_json_kb_sum ),
                        static_cast<unsigned long long>( rsp_prof.ev_push_count ),
                        static_cast<unsigned long long>( rsp_prof.anon_mb_sample ),
                        static_cast<unsigned long long>( rsp_prof.avframe_drop_total ),
                        static_cast<unsigned long long>( rsp_prof.inflight_drop_total ),
                        static_cast<unsigned long long>( rsp_prof.event_drop_total ),
                        static_cast<unsigned long long>( rsp_prof.io_drop_total ) );
                    rsp_prof = RspProf{};
                }
            }
#endif
        }
        MLOG_INFO("CAM[%d] ResponseThreadRunner finished", id_);
    }

    void RtspDetectorUnit::ResponseThreadCloser( void )
    {
        // InferenceThreadCloser 에서 이미 inflight_q_->terminate() 호출됨 — race-free.
        if( inflight_q_ ) {
            inflight_q_->clear_without_action();
        }
        // B3 — RspThread 가 종료되면 event_q producer 정지. EvtThread 가 dequeue 종료할 수 있도록 terminate.
        if( event_q_ ) {
            event_q_->terminate();
        }
    }

    void RtspDetectorUnit::EventThreadCloser( void )
    {
        // ResponseThreadCloser 에서 이미 event_q_->terminate() 호출됨 — race-free.
        if( event_q_ ) {
            event_q_->clear_without_action();
        }
    }

    void RtspDetectorUnit::EventThreadRunner( void )
    {
        using namespace std::chrono_literals;

        MGEN::CorrelationContext::Set( "sys-event-" + std::to_string( this->id_ ) );
        auto& running = this->event_thread_.GetRunningFlag();

        // EvtThread 자체 origin_ctx — RspThread 의 resp_origin_ctx 와 분리.
        FrameFormattingContext evt_origin_ctx;
        EventTime last_interval_log_print_time = std::chrono::system_clock::now();
        const auto long_timelapse_log_interval = 10min;

#ifdef DEBUG_MODE
        // v7 — EvtThread stage timing instrument.
        // Release 빌드에선 본 영역 전체 preprocessor 제거.
        struct EvtProf {
            uint64_t pop_us           = 0;   // event_q dequeue 시간 (대부분 wait, frame in 따라)
            uint64_t sched_us         = 0;   // scheduler->Check loop + BuildNotifyJsonImpl_Analysis (event_list 만들기)
            uint64_t conv_us          = 0;   // Convert + clone + io_work_queue push
            uint64_t sio_us           = 0;   // sio_emit loop (event_list 별)
            uint64_t grpc_us          = 0;   // grpc send loop
            uint64_t event_q_size_sum = 0;   // pop 직전 event_q size (sample)
            uint64_t event_q_size_max = 0;
            uint64_t io_q_size_sum    = 0;   // io_work_queue push 직전 size (sample)
            uint64_t io_q_size_max    = 0;
            uint64_t json_bytes_sum   = 0;   // event_msg_json dump size 누적 (모든 event)
            uint64_t json_bytes_max   = 0;
            uint64_t event_list_size_sum = 0;  // event_list (per cycle) size 누적
            uint64_t event_list_size_max = 0;
            uint64_t sio_emit_count   = 0;
            uint64_t grpc_send_count  = 0;
            uint64_t empty_cycle_count = 0;  // event_list 비어서 skip 한 cycle (no_schedule 또는 trigger 없음)
            uint64_t event_drop_total = 0;   // event_q drop counter snapshot
            uint64_t io_drop_total    = 0;
            uint64_t count            = 0;   // event_list 가 있어 진짜 event 처리 한 cycle
        };
        EvtProf evt_prof;

        auto evt_us = []( const std::chrono::steady_clock::time_point& a,
                          const std::chrono::steady_clock::time_point& b ) -> uint64_t {
            return static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>( b - a ).count() );
        };
#endif

        while( running.load() == true )
        {
            if( !event_q_ ) {
                std::this_thread::sleep_for( 10ms );
                continue;
            }

            DBG_PROF( const auto t_evt_top = std::chrono::steady_clock::now(); )
            DBG_PROF( const size_t cyc_event_q_size = event_q_->size(); )  // v7 — pop 직전 size sample

            auto opt_item = event_q_->dequeue_wait_for( 100ms );
            DBG_PROF( const auto t_after_pop = std::chrono::steady_clock::now(); )

            if( !opt_item.has_value() ) {
                if( event_q_->is_terminated() ) break;
                continue;
            }
            const EventItem item = std::move( *opt_item );
            auto& frame         = item.frame;
            auto& track_results = item.track_results;
            if( !frame ) continue;

            // ─────────────────────────────────────────────────────────────
            // 1. schedule check + BuildNotifyJsonImpl (event_list 만들기)
            // ─────────────────────────────────────────────────────────────
            if( scheduler_.empty() ){
                EventTime now_sys = std::chrono::system_clock::now();
                if( IsOverTime( now_sys, last_interval_log_print_time, long_timelapse_log_interval ) ){
                    MLOG_INFO("CAM[%d] AI inference detected ( %4zu ojbect ), but target camera has not exist EVENT schedule.",
                        id_, track_results.size() );
                    last_interval_log_print_time = now_sys;
                }
                DBG_PROF( evt_prof.empty_cycle_count += 1; )
                continue;
            }

            std::vector<ScheduleEventDTO> event_list {};
            std::string frame_path {};
            event_list.reserve( scheduler_.size() );
            frame_path.reserve( 128 );

            if( auto opt = MakeImageSavePath( GetFrameImageCurrentProxyRootPath() ); opt.has_value() ){
                frame_path = *opt;
            }
            else {
                MLOG_ERROR( "%s() => CAM[%d] make frame image save directory failed", __func__, id_ );
                DBG_PROF( evt_prof.empty_cycle_count += 1; )
                continue;
            }

            struct tm tstruct {};
            auto      curr_time = std::time( nullptr );
            auto*     time_info = localtime_r( &curr_time, &tstruct );

            DBG_PROF( uint64_t cyc_json_bytes = 0; )
            for( const auto& each_schedule : scheduler_ )
            {
                const std::vector<InferObject> on_event_results = each_schedule->Check( track_results );
                if( on_event_results.empty() ) continue;

                {
                    const std::string ev_name = Abnormal::Schedule::GetEventName( each_schedule->sch_info.event_code );
                    MGEN::MetricsRegistry::Instance().IncrementCounter(
                        "detectbase_events_total",
                        { { "type", ev_name }, { "cam", std::to_string( id_ ) } },
                        static_cast<double>( on_event_results.size() ) );
#ifdef DEBUG_MODE
                    // event_detected MLOG_INFO — Release 빌드에선 compile-out (Prometheus counter 만 운영 유지).
                    MLOG_INFO( "event_detected type=%s cam=%d count=%zu",
                        ev_name.c_str(), id_, on_event_results.size() );
#endif
                }

                auto event_msg_json = this->BuildNotifyJsonImpl_Analysis( each_schedule, on_event_results, time_info, frame_path );
                DBG_PROF( cyc_json_bytes += static_cast<uint64_t>( event_msg_json.dump().size() ); )
                event_list.push_back( ScheduleEventDTO{ std::move( event_msg_json ), on_event_results } );
            }
            DBG_PROF( const auto t_after_sched = std::chrono::steady_clock::now(); )

            if( event_list.empty() ) {
#ifdef DEBUG_MODE
                // schedule trigger 안 됨 cycle — sched timing 만 누적
                evt_prof.pop_us           += evt_us( t_evt_top,  t_after_pop  );
                evt_prof.sched_us         += evt_us( t_after_pop, t_after_sched );
                evt_prof.event_q_size_sum += cyc_event_q_size;
                if( cyc_event_q_size > evt_prof.event_q_size_max ) evt_prof.event_q_size_max = cyc_event_q_size;
                evt_prof.empty_cycle_count += 1;
#endif
                continue;
            }

            // ─────────────────────────────────────────────────────────────
            // 2. Convert + clone + io_work_queue push (event 발생 시만)
            // ─────────────────────────────────────────────────────────────
            if( !evt_origin_ctx.Update( frame->width, frame->height, frame->width, frame->height, frame->format ) ){
                MLOG_ERROR("CAM[%d] Event | evt_origin_ctx update failed", id_);
            }
            if( evt_origin_ctx.Convert( frame, true, false ) == false ){
                MLOG_ERROR("CAM[%d] Event | Origin Image Convert failed", id_);
                continue;
            }

            const size_t cyc_io_q_size = io_work_queue_ ? io_work_queue_->size() : 0;
            if( io_work_queue_ ){
                IOWorkItem io_item;
                io_item.frame_path = frame_path;
                io_item.image_mat  = evt_origin_ctx.save_snapshot_mat.clone();
                if( cyc_io_q_size >= 30 ) {
                    MGEN::MetricsRegistry::Instance().IncrementCounter(
                        "detectbase_errors_total", { { "type", "io_work_drop" } } );
                }
                io_work_queue_->enqueue_move( std::move( io_item ) );
            }
            DBG_PROF( const auto t_after_conv = std::chrono::steady_clock::now(); )

            // ─────────────────────────────────────────────────────────────
            // 3. sio emit (event_list 별)
            // ─────────────────────────────────────────────────────────────
            DBG_PROF( uint64_t cyc_sio_emit = 0; )
            if( sio_handler_ )
            {
                for( auto& target_event : event_list )
                {
                    sio_handler_->Emit( std::string { SocketIO::EventName::DETECTOR_MESSAGE }, target_event.event_message );
                    DBG_PROF( cyc_sio_emit += 1; )
                }
            }
            DBG_PROF( const auto t_after_sio = std::chrono::steady_clock::now(); )

            // ─────────────────────────────────────────────────────────────
            // 4. grpc send
            // ─────────────────────────────────────────────────────────────
            DBG_PROF( uint64_t cyc_grpc_send = 0; )
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
                        DBG_PROF( cyc_grpc_send += static_cast<uint64_t>( sent ); )
                    }
                }
            }
            DBG_PROF( const auto t_after_grpc = std::chrono::steady_clock::now(); )

#ifdef DEBUG_MODE
            // ─────────────────────────────────────────────────────────────
            // v7 — EvtProf flush 누적 + 100 cycle 마다 log
            // ─────────────────────────────────────────────────────────────
            evt_prof.pop_us           += evt_us( t_evt_top,    t_after_pop   );
            evt_prof.sched_us         += evt_us( t_after_pop,  t_after_sched );
            evt_prof.conv_us          += evt_us( t_after_sched, t_after_conv );
            evt_prof.sio_us           += evt_us( t_after_conv,  t_after_sio  );
            evt_prof.grpc_us          += evt_us( t_after_sio,   t_after_grpc );
            evt_prof.event_q_size_sum += cyc_event_q_size;
            if( cyc_event_q_size > evt_prof.event_q_size_max ) evt_prof.event_q_size_max = cyc_event_q_size;
            evt_prof.io_q_size_sum    += cyc_io_q_size;
            if( cyc_io_q_size > evt_prof.io_q_size_max ) evt_prof.io_q_size_max = cyc_io_q_size;
            evt_prof.json_bytes_sum   += cyc_json_bytes;
            if( cyc_json_bytes > evt_prof.json_bytes_max ) evt_prof.json_bytes_max = cyc_json_bytes;
            evt_prof.event_list_size_sum += event_list.size();
            if( event_list.size() > evt_prof.event_list_size_max ) evt_prof.event_list_size_max = event_list.size();
            evt_prof.sio_emit_count   += cyc_sio_emit;
            evt_prof.grpc_send_count  += cyc_grpc_send;
            evt_prof.count            += 1;

            if( evt_prof.count >= 100 ) {
                evt_prof.event_drop_total = event_q_       ? event_q_->GetDropCount()       : 0;
                evt_prof.io_drop_total    = io_work_queue_ ? io_work_queue_->GetDropCount() : 0;
                const uint64_t denom = evt_prof.count + evt_prof.empty_cycle_count;  // queue size 평균은 모든 cycle 기준
                MLOG_INFO("CAM[%d] EVT-thread (avg over %llu event cycles + %llu empty cycles, us): pop=%llu sched=%llu conv=%llu sio=%llu grpc=%llu | event_q avg=%llu max=%llu | io_q avg=%llu max=%llu | json_bytes avg=%llu max=%llu | event_list avg=%llu max=%llu | sio_emit=%llu grpc_send=%llu | q_drop ev=%llu io=%llu",
                    id_,
                    static_cast<unsigned long long>( evt_prof.count ),
                    static_cast<unsigned long long>( evt_prof.empty_cycle_count ),
                    static_cast<unsigned long long>( evt_prof.pop_us   / std::max<uint64_t>( denom, 1 ) ),
                    static_cast<unsigned long long>( evt_prof.sched_us / std::max<uint64_t>( denom, 1 ) ),
                    static_cast<unsigned long long>( evt_prof.conv_us  / evt_prof.count ),
                    static_cast<unsigned long long>( evt_prof.sio_us   / evt_prof.count ),
                    static_cast<unsigned long long>( evt_prof.grpc_us  / evt_prof.count ),
                    static_cast<unsigned long long>( evt_prof.event_q_size_sum / std::max<uint64_t>( denom, 1 ) ),
                    static_cast<unsigned long long>( evt_prof.event_q_size_max ),
                    static_cast<unsigned long long>( evt_prof.io_q_size_sum / evt_prof.count ),
                    static_cast<unsigned long long>( evt_prof.io_q_size_max ),
                    static_cast<unsigned long long>( evt_prof.json_bytes_sum / evt_prof.count ),
                    static_cast<unsigned long long>( evt_prof.json_bytes_max ),
                    static_cast<unsigned long long>( evt_prof.event_list_size_sum / evt_prof.count ),
                    static_cast<unsigned long long>( evt_prof.event_list_size_max ),
                    static_cast<unsigned long long>( evt_prof.sio_emit_count ),
                    static_cast<unsigned long long>( evt_prof.grpc_send_count ),
                    static_cast<unsigned long long>( evt_prof.event_drop_total ),
                    static_cast<unsigned long long>( evt_prof.io_drop_total ) );
                evt_prof = EvtProf{};
            }
#endif
        }
        MLOG_INFO("CAM[%d] EventThreadRunner finished", id_);
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
