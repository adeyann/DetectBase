#pragma once

// ---------------------------------------------------------------------------
// 1. Framework & BasicLibs Headers
// ---------------------------------------------------------------------------
#include "ServiceBlockProfile.h"
#include "SafeThread.h"
#include "SafeQueue.h"
#include "MgenTypes.h"
#include "EngineStreamTypes.h"
#include "MgenSchedule.h"
#include "SettingMonitor.h"
#include "SettingManager.h"
#include "json/json_fwd.hpp"

// ---------------------------------------------------------------------------
// 2. New Common Modules (VisionCommon & Management)
// ---------------------------------------------------------------------------
#include "SORT/SORTTracker.h"       // Tracker 모듈

// ---------------------------------------------------------------------------
// 3. Network & IO Headers
// ---------------------------------------------------------------------------
#include "IOStreamManager.h"
#include "NetworkManager.h"
#include "EngineLoadBalancer.h"
#include "rtsp_proxy.h"

// STL
#include <mutex>
#include <atomic>
#include <string_view>
#include <map>

namespace MGEN
{
    namespace
    {
        // AI Engine 매직 이름 (단일 Detection 엔진 구성).
        // 값은 engine.profile.json 의 "ModelMagicType" 과 일치해야 함.
        constexpr std::string_view MAGIC_DETECTION_ENGINE_NAME = "DetectionEngine";

        // 재초기화를 트리거하기 위한 연속 불일치 프레임 임계값
        constexpr size_t REINIT_THRESHOLD_COUNT = 10;
    }

    struct ScheduleEventDTO
    {
        nlohmann::json           event_message;
        std::vector<InferObject> on_event_results;
    };

    class RtspDetectorUnit final : private SettingMonitor
    {
    public:
        // Default constructor delete
        RtspDetectorUnit() = delete;

        // Base constructor
        explicit RtspDetectorUnit(
            const MGEN::Type::UnitID            service_unit_id,
            const ServiceBlockProfile&          service_block_profile,
            std::shared_ptr<NetworkManager>     network_manager,
            std::shared_ptr<IOStreamManager>    io_stream_manager,
            std::shared_ptr<EngineLoadBalancer> load_balancer
        );

        // Destructor
        ~RtspDetectorUnit();

        bool Init ( void );
        bool Start( void );
        bool Stop ( void );

        // Getter
        MGEN::Type::UnitID GetServiceID( void ) const noexcept { return id_; }

    private:
        // Thread functions
        void InferenceThreadRunner( void );
        void InferenceThreadCloser( void );

        // IO Worker (cv::imwrite 비동기 처리). 이벤트 빈발 시 main loop 의 frame drop 차단.
        void IOWorkerThreadRunner( void );
        void IOWorkerThreadCloser( void );

        // IO Worker 작업 단위. main loop 가 enqueue, IO worker thread 가 dequeue.
        struct IOWorkItem
        {
            std::string frame_path;
            cv::Mat     image_mat; // origin_ctx.save_snapshot_mat 의 clone (deep copy 필수)
        };

        // Internal helper - Engine check
        bool IsLoadable( std::string_view target_engine_magic_name ) const;

        // Internal helper - Save images
        std::string GetFrameImageCurrentProxyRootPath( void ) const;
        std::optional<std::string> MakeImageSavePath( const std::string& root_path ) const;

        // RTSP metadata
        void SendDetectResultToMetaData( const std::vector<InferObject>& detect_results );

        // For Socket.io message - base build
        nlohmann::json BuildNotifyJsonBase(
            const std::string&               message_type, // Socket.io emit event name (namespace)
            const SocketIO::TransmissionType trans_type,   // braodcast or unicast
            const tm*                        time_info,
            const std::string&               destination = std::string{""}
        );

        // For Socket.io message - 선별(Analysis) 부분 ( Detection )
        nlohmann::json BuildNotifyJsonImpl_Analysis(
            std::shared_ptr<Abnormal::Schedule>   event_occured_schedule,
            const std::vector<MGEN::InferObject>& on_event_results,
            const tm*                             event_occur_time_info,
            const std::string&                    frame_image_path
        );

        // Internal Helper - for release & reset
        void ReleaseSchedules( void );
        void ResetTrackers( void );

    private:
        // const
        const MGEN::Type::UnitID id_ = UNIT_ID_NOT_SET;

        // RTSP Proxy ptr for link video_decoder
        CRtspProxy* proxy_ptr_ = nullptr;

        // Thread for detection & attribute inference -- Main Thread
        SafeThread inference_thread_;
        sptrSafeQueue<std::shared_ptr<AVFrame>> avframe_q_ = nullptr;

        // Stage G (cv::imwrite) 비동기 처리. main loop 가 enqueue 만 → main 의 frame period 보호.
        SafeThread                              io_worker_thread_;
        std::unique_ptr<SafeQueue<IOWorkItem>>  io_work_queue_     = nullptr;

        // Extern manager modules
        std::shared_ptr<EngineLoadBalancer> load_balancer_    = nullptr;
        std::shared_ptr<NetworkManager>     network_manager_  = nullptr;
        std::shared_ptr<IOStreamManager>    iostream_manager_ = nullptr;
        std::shared_ptr<RtspHandler>        rtsp_handler_     = nullptr;
        std::shared_ptr<SioHandler>         sio_handler_      = nullptr;

        // Tracker
        std::unordered_map<MGEN::InferClassID, std::unique_ptr<SORTTracker>> trackers_;
        std::unordered_map<MGEN::InferClassID, unsigned int>                 tracker_seqs_;

        // atmoic frame count
        std::atomic<unsigned int> frame_count_ { 0 };

        // For Init check
        bool is_initialized_service_ = false;

        // 연속적인 프레임 해상도 불일치 횟수 카운터
        size_t consecutive_mismatch_count_ = 0;

        // stable video input frame w x h
        int stable_frame_w_ = 0;
        int stable_frame_h_ = 0;

        // for manage subscribe ids
        std::set<MGEN::Type::UnitID> subscribe_ids_;

        // Settings - Exceptions
        ExcludeCamSettingData exclude_setting_;
        mutable std::mutex    exclude_setting_mtx_;

        // Setting - Schedules
        ScheduleSettingData schedule_settings_;
        mutable std::mutex  schedule_settings_mtx;
        std::atomic<bool>   is_schedule_updated_ { false };

        // Abnormal actions ( schedule )
        std::vector<std::shared_ptr<Abnormal::Schedule>> scheduler_;

        // Internal cache variables to prevent too frequent loading
        std::string proxy_url_           = "";
        int         socket_io_server_id_ = DefineDefault::SERVER_IDENTIFIER_NOT_SET;
        int         detect_fps_limit_    = 30;
        int         realtime_fps_        = 30;
    };

}