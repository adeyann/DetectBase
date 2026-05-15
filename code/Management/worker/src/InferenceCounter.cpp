#include "InferenceCounter.h"
#include "MgenLogger.h"
#include "MetricsRegistry.h"  // P54: dfps / camera_count gauge update
#include "SettingData.h"      // P61: DefineDefault::FULL_FRAME_IMAGE_SAVE_ROOT_PATH

#include "EngineStreamTypes.h"

#include <sys/statvfs.h>      // P54 Layer 3: /frame disk usage 메트릭

namespace MGEN
{
    InferenceCounter::InferenceCounter()
    {
        this->thread_.SetThreadFunctions(
            // runner
            std::bind( &InferenceCounter::InferenceCounterThreadRunner, this ),
            // closer
            std::bind( &InferenceCounter::InferenceCounterThreadCloser, this )
        );
    }

    InferenceCounter::~InferenceCounter()
    {
        this->Stop();
    }

    void InferenceCounter::Start( void )
    {
        this->thread_.Start();
    }

    void InferenceCounter::Stop( void )
    {
        this->thread_.Stop();
    }

    void InferenceCounter::Regist( const MGEN::Type::UnitID unit_id )
    {
        const int target_id = unit_id;

        if( this->counters_.find( target_id ) == this->counters_.end() )
            this->counters_[target_id] = 0;
    }

    void InferenceCounter::Unregist( const MGEN::Type::UnitID unit_id )
    {
        const int target_id = unit_id;

        if( this->counters_.find( target_id ) != this->counters_.end() )
            this->counters_.erase( target_id );
    }

    void InferenceCounter::AddCount( const MGEN::Type::UnitID unit_id )
    {
        const int target_id = unit_id;

        if( this->counters_.find( target_id ) == this->counters_.end() )
            return;

        ( this->counters_[target_id] ).fetch_add(1);
    }

    void InferenceCounter::InferenceCounterThreadRunner( void )
    {
        auto&  running = this->thread_.GetRunningFlag();
        while( running.load() == true )
        {
            // 1. 전체 등록된 카메라 ID 목록 확보 (Undetected 계산을 위해 필요)
            std::set<int> all_regist_ids;
            for( auto& [ unit_id, each_counter ] : this->counters_ ){
                all_regist_ids.insert( GetPureIDFromInferenceRequesterID( unit_id ) );
            }

            const size_t total_regist_cam_count = all_regist_ids.size();

            if( total_regist_cam_count == 0 ) {
                std::this_thread::sleep_for( std::chrono::seconds( DFPS_CHECK_INTERVAL_SECONDS / 10 ) );
                continue;
            }

            // 2. 집계를 위한 데이터 구조 초기화 (DETECTOR 단일 분기)
            unsigned int           main_total_frames = 0;
            std::set<Type::UnitID> main_active_cam_ids;

            // 3. 카운터 순회 및 데이터 수집
            for( auto& [ unit_id, each_counter ] : this->counters_ )
            {
                // load + store(0) atomic operation
                const auto count = each_counter.exchange(0);

                // 처리 건수 없으면 스킵
                if( count == 0 )
                    continue;

                // ID 파싱
                const auto engine_mod  = GetModelMajorTypeFromEngineIdentifier( unit_id );
                const auto pure_cam_id = GetPureIDFromInferenceRequesterID( unit_id ); // 순수 카메라 ID

                if( engine_mod == ModelMajorType::Detection ) {
                    main_total_frames += count;
                    main_active_cam_ids.insert( pure_cam_id );
                }
            }

            // 4. 로그 출력 (DFPS - Detection FPS)
            const size_t active_cnt = main_active_cam_ids.size();
            const int    interval   = (DFPS_CHECK_INTERVAL_SECONDS > 0) ? DFPS_CHECK_INTERVAL_SECONDS : 1;

            // 엔진 갯수(coef)로 나누지 않음. 실제 처리량(Raw Load) 계산.
            const float avg_fps_per_cam = ( active_cnt == 0 )
                                        ? 0.0f
                                        : static_cast<float>(main_total_frames) / static_cast<float>(active_cnt * interval);

            const float total_fps_raw   = static_cast<float>(main_total_frames) / static_cast<float>(interval);

            std::string undetected_list_str = "";
            if( active_cnt < total_regist_cam_count )
            {
                for( const auto& id : all_regist_ids ){
                    if( main_active_cam_ids.find(id) == main_active_cam_ids.end() ){
                        undetected_list_str += std::to_string(id) + " ";
                    }
                }
            }
            MLOG_INFO( "[DFPS] %4.1f FPS/cam ( TotalDFPS: %6.1f | RegistCam: %04zu | RequestCam: %04zu ) - NotRequest [ %s]",
                avg_fps_per_cam, total_fps_raw,
                total_regist_cam_count, active_cnt, undetected_list_str.c_str() );

            // P54: 메트릭 update (Prometheus exporter).
            {
                auto& m = MGEN::MetricsRegistry::Instance();
                m.SetGauge( "detectbase_dfps_total", {}, total_fps_raw );
                m.SetGauge( "detectbase_camera_count", { { "state", "registered" } },
                    static_cast<double>( total_regist_cam_count ) );
                m.SetGauge( "detectbase_camera_count", { { "state", "active" } },
                    static_cast<double>( active_cnt ) );

                // P54 Layer 3: /frame 디스크 사용량 메트릭. statvfs 실패 시 skip.
                // P61: DefineDefault 의 단일 진실 사용.
                struct statvfs s {};
                if( ::statvfs( DefineDefault::FULL_FRAME_IMAGE_SAVE_ROOT_PATH, &s ) == 0 && s.f_blocks > 0 ) {
                    const double frsize = static_cast<double>( s.f_frsize );
                    const double cap    = static_cast<double>( s.f_blocks ) * frsize;
                    const double avail  = static_cast<double>( s.f_bavail ) * frsize;
                    const double used   = cap - avail;
                    const double pct    = 100.0 * used / cap;
                    m.SetGauge( "detectbase_frame_disk_capacity_bytes", {}, cap );
                    m.SetGauge( "detectbase_frame_disk_used_bytes",     {}, used );
                    m.SetGauge( "detectbase_frame_disk_used_pct",       {}, pct );
                }
            }

            // 5. Wait
            std::unique_lock<std::mutex> lck { this->mutex_ };
            this->cond_.wait_for( lck, std::chrono::seconds( DFPS_CHECK_INTERVAL_SECONDS ), [&] { return running.load() == false; } );

            if( running.load() == false )
                break;
        }
    }

    void InferenceCounter::InferenceCounterThreadCloser( void )
    {
        std::unique_lock<std::mutex> lck { this->mutex_ };
        this->cond_.notify_all();
    }

} // namespace MGEN
