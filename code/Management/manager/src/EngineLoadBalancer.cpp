#include "EngineLoadBalancer.h"
#include "MgenLogger.h"
#include "MetricsRegistry.h"
#include "MgenTypes.h"

#include <vector>
#include <chrono>

namespace MGEN
{
    // --- Static Helper ---

    /**
     * @brief EngineProfile 벡터 → InferEngineID 키 맵
     */
    static std::unordered_map<InferEngineID, EngineProfile> CreateProfileMap( const std::vector<EngineProfile>& profiles )
    {
        std::unordered_map<InferEngineID, EngineProfile> map;
        map.reserve( profiles.size() );
        for ( const auto& profile : profiles ){
            map.emplace( profile.GetProfileUUID(), profile );
        }
        return map;
    }

    // --- Constructor / Destructor ---

    EngineLoadBalancer::EngineLoadBalancer( const std::vector<EngineProfile>& profiles ) noexcept
        : engine_profiles_       ( CreateProfileMap( profiles ) )
        , infer_respond_receiver_( std::make_shared<SafeQueue<OutputLayerWrapper>>() )
        , is_terminate_instance_ ( false )
    {
        MLOG_DEBUG("EngineLoadBalancer initializing...");

        this->reply_dispatcher_thread_.SetThreadFunctions(
            std::bind( &EngineLoadBalancer::ReplyDispatcherThreadRunner, this ),
            std::bind( &EngineLoadBalancer::ReplyDispatcherThreadCloser, this )
        );
        this->reply_dispatcher_thread_.Start();

        // 응답 처리 디스패처 auto cleanup 삭제 주기 10초, 검사 주기 5초
        this->reply_dispatcher_.StartAutoCleanup( 10'000, 5'000 );

        MLOG_DEBUG("EngineLoadBalancer initialized successfully.");
    }

    EngineLoadBalancer::~EngineLoadBalancer()
    {
        this->Terminate();
    }

    // --- Public Methods ---

    bool EngineLoadBalancer::Terminate( void )
    {
        if ( this->is_terminate_instance_.exchange( true ) == false )
        {
            this->reply_dispatcher_.StopAutoCleanup();
            this->reply_dispatcher_thread_.Stop();
            MLOG_INFO( "   - Engine Inference Reply Dispatcher Thread Terminated" );

            this->inference_counter_.Stop();
            MLOG_INFO( "   - Inference Counter Stopped" );

            // input queue shared_ptr 명시 release (handler 별 큐 모두 정리).
            // EngineHandlerBase 가 동일 큐를 보유하므로 메모리는 자연 정리되지만,
            // Terminate 시점에 반환을 명확히 해서 정리 시점 모호성 제거.
            {
                std::lock_guard<std::mutex> lck { this->engine_mutex_ };
                this->engine_input_qs_.clear();
                this->engine_handles_.clear();
            }

            return true;
        }
        return false;
    }

    bool EngineLoadBalancer::IsLoadEngine( void ) const noexcept
    {
        if ( is_terminate_instance_.load() == true )
            return false;

        std::lock_guard<std::mutex> lck { this->engine_mutex_ };
        return !this->engine_input_qs_.empty();
    }

    InferEngineID EngineLoadBalancer::GetAvailableInferEngineID( const InferRequestRequireOneType& require ) const noexcept
    {
        const auto candidates = SearchEngineUUID( this->engine_profiles_, require );
        if ( candidates.empty() )
            return INFER_ENGINE_ID_NOT_SET;
        return candidates.front();
    }

    std::optional<EngineProfile> EngineLoadBalancer::GetEngineProfileTargetInferEngineID( const InferEngineID id ) const noexcept
    {
        auto profile_iter = this->engine_profiles_.find( id );
        if ( profile_iter == this->engine_profiles_.end() )
            return std::nullopt;
        return std::make_optional<EngineProfile>( profile_iter->second );
    }

    bool EngineLoadBalancer::Subscribe( const MGEN::Type::UnitID unit_id ) noexcept
    {
        if ( is_terminate_instance_.load() == true )
            return false;

        std::lock_guard<std::mutex> lck { this->unit_regist_mutex_ };

        if ( this->subscribers_.count( unit_id ) > 0 ){
            MLOG_WARN("Unit ID %d already subscribed.", unit_id);
            return false;
        }

        if ( this->reply_dispatcher_.is_exist_in_entry( unit_id ) ){
            MLOG_WARN("Unit ID %d already exists in reply dispatcher.", unit_id);
        }

        this->subscribers_.insert( unit_id );
        this->inference_counter_.Regist( unit_id );

        return true;
    }

    void EngineLoadBalancer::Unsubscribe( const MGEN::Type::UnitID unit_id ) noexcept
    {
        std::lock_guard<std::mutex> lck { this->unit_regist_mutex_ };
        if ( this->subscribers_.find( unit_id ) != this->subscribers_.end() ){
            this->inference_counter_.Unregist( unit_id );
            this->subscribers_.erase( unit_id );
        }
    }

    sptrSafeQueue<OutputLayerWrapper> EngineLoadBalancer::Linker( const EngineHandleUUID& handle_uuid, sptrSafeQueue<InputLayerWrapper> input_q )
    {
        const InferEngineID target_engine_id = GetInferEngineID( handle_uuid );
        const InferDeviceID target_device_id = GetInferDeviceID( handle_uuid );
        MLOG_DEBUG("Linking engine handler: Engine ID %d, Device ID %d", target_engine_id, target_device_id);

        std::lock_guard<std::mutex> lck { this->engine_mutex_ };

        if ( is_terminate_instance_.load() == true )
            return nullptr;

        if ( this->engine_input_qs_.count( handle_uuid ) ){
            MLOG_WARN("Engine handler (%d, %d) already linked — duplicate Link rejected.",
                target_engine_id, target_device_id);
            return nullptr;
        }

        if ( this->engine_profiles_.find( target_engine_id ) == this->engine_profiles_.end() ){
            MLOG_ERROR("%s() => Profile not found for engine ID %d.", __func__, target_engine_id);
            return nullptr;
        }

        this->engine_input_qs_[ handle_uuid ] = input_q;
        this->engine_handles_.push_back( handle_uuid );

        MLOG_INFO("Engine handler (%d, %d) linked successfully. Total handlers: %zu",
            target_engine_id, target_device_id, this->engine_handles_.size());
        return this->infer_respond_receiver_;
    }

    EngineLinker EngineLoadBalancer::GetEngineLinker( void )
    {
        return std::bind( &EngineLoadBalancer::Linker, this, std::placeholders::_1, std::placeholders::_2 );
    }

    bool EngineLoadBalancer::RequestAsync( InputLayerWrapper&& request )
    {
        if ( is_terminate_instance_.load() == true )
            return false;

        if ( !request.image_data || request.image_data->empty() ){
            MLOG_WARN("Received inference request with empty image data. Ignoring.");
            return false;
        }

        const MGEN::Type::UnitID requester_unit_id = request.meta_data.requester_unit_id;

        sptrSafeQueue<InputLayerWrapper> target_queue = nullptr;
        EngineHandleUUID                 target_handle;
        {
            std::lock_guard<std::mutex> lck { this->engine_mutex_ };

            if ( this->engine_handles_.empty() ){
                MLOG_WARN("Unit (%d) Request Inference, but AI engine isn't loaded yet.", requester_unit_id);
                return false;
            }

            // Round-robin handler selection — atomic counter % N. 균등 분산 (least-loaded 보다 단순,
            // response-side usage tracking 불필요. NPU bottleneck 환경에서 큐 길이는 비슷하게 유지됨).
            const size_t idx = this->round_robin_idx_.fetch_add( 1, std::memory_order_relaxed )
                               % this->engine_handles_.size();
            target_handle = this->engine_handles_[ idx ];
            target_queue  = this->engine_input_qs_[ target_handle ];
        }

        // [Backpressure] 큐가 꽉 찼으면 즉시 drop (RTSP 실시간성 보호)
        if ( target_queue->size() >= MAX_ENGINE_INFER_INPUT_QUEUE_SIZE ){
            MLOG_WARN("Engine Input Queue Full (%zu/%d). Dropping request to prevent lag.",
                target_queue->size(), MAX_ENGINE_INFER_INPUT_QUEUE_SIZE);
            MGEN::MetricsRegistry::Instance().IncrementCounter(
                "detectbase_errors_total", { { "type", "engine_input_q_drop" } } );
            return false;
        }

        // 선택된 handler 의 engine id 로 정렬
        request.meta_data.requestee_engine_uuid = target_handle.first;

        target_queue->enqueue_move( std::move( request ) );
        return true;
    }

    std::optional<OutputLayerWrapper> EngineLoadBalancer::RespondAsync( const MGEN::Type::UnitID unit_id, const int timeout_ms )
    {
        {
            std::lock_guard<std::mutex> lck { this->unit_regist_mutex_ };
            if ( this->subscribers_.find( unit_id ) == this->subscribers_.end() )
                return std::nullopt;
        }
        return this->reply_dispatcher_.wait_and_get( unit_id, timeout_ms );
    }

    std::optional<OutputLayerWrapper> EngineLoadBalancer::Request( InputLayerWrapper&& request, const int timeout_ms )
    {
        if ( !request.image_data || request.image_data->empty() ){
            return std::nullopt;
        }
        const MGEN::Type::UnitID unit_id = request.meta_data.requester_unit_id;
        if ( !this->RequestAsync( std::move( request ) ) ){
            return std::nullopt;
        }
        return this->RespondAsync( unit_id, timeout_ms );
    }

    void EngineLoadBalancer::DestroySubscribersAll( void )
    {
        MLOG_INFO("Destroying all subscribers...");
        this->reply_dispatcher_.terminate();
        {
            std::lock_guard<std::mutex> lck { this->unit_regist_mutex_ };
            this->subscribers_.clear();
        }
        MLOG_INFO("All subscribers destroyed.");
    }

    void EngineLoadBalancer::AddInferCount( const MGEN::Type::UnitID id )
    {
        this->inference_counter_.AddCount( id );
    }

    void EngineLoadBalancer::StartInferenceCounter( void ) { this->inference_counter_.Start(); }
    void EngineLoadBalancer::StopInferenceCounter ( void ) { this->inference_counter_.Stop();  }

    // --- Private Methods ---

    void EngineLoadBalancer::ReplyDispatcherThreadRunner( void )
    {
        using namespace std::chrono_literals;

        auto& running = this->reply_dispatcher_thread_.GetRunningFlag();
        while ( running.load() )
        {
            // dequeue_wait_for 는 종료/타임아웃 시 std::nullopt 반환 (throw 없음)
            auto opt_respond = this->infer_respond_receiver_->dequeue_wait_for( 1s );
            if( !opt_respond.has_value() ){
                if( this->infer_respond_receiver_->is_terminated() ){
                    MLOG_INFO("   - Reply dispatcher queue terminated. Stopping runner.");
                    break;
                }
                continue; // timeout → 다시 wait
            }

            // 응답을 unit_id 별 ReplyDispatcher 로 분배
            this->reply_dispatcher_.set_reply( opt_respond->meta_data.requester_unit_id, *opt_respond );
        }
        MLOG_INFO("   - ReplyDispatcherThreadRunner finished.");
    }

    void EngineLoadBalancer::ReplyDispatcherThreadCloser( void )
    {
        this->infer_respond_receiver_->terminate();
        MLOG_INFO( "   - Inference Observer Queue Flushed ( remains : %zu )", this->infer_respond_receiver_->size() );
    }

} // namespace MGEN
