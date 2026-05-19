#include "EngineHandlerBase.h"
#include "MgenLogger.h"

#include <numeric>      // std::accumulate 등 (필요 시)
#include <stdexcept>    // std::runtime_error (예외 처리)

namespace MGEN
{
    EngineHandlerBase::EngineHandlerBase( const EngineProfile& profile, const InferDeviceID device_id ) noexcept
        : profile_   ( profile )
        , device_id_ ( device_id )
        , batch_size_( profile.GetInferenceBatchSize() )
    {
        //
    }

    EngineHandlerBase::~EngineHandlerBase()
    {
        // 아직 실행 중이면 TerminateEngine 호출 보장
        if( is_activate_handler_.load() == true ){
            MLOG_WARN("EngineHandlerBase destructor called while engine '%s' on device %d was still active. Forcing termination.",
                profile_.GetProfileName().c_str(), device_id_);
            this->TerminateEngine();
        }
        MLOG_DEBUG("EngineHandlerBase instance destroyed for profile '%s' (UUID: %d) on device %d.",
            profile_.GetProfileName().c_str(), profile_.GetProfileUUID(), device_id_);
    }

    bool EngineHandlerBase::ActivateEngine( void )
    {
        if( is_activate_handler_.load() == true ){
            MLOG_WARN("Engine '%s' on device %d is already activated.", profile_.GetProfileName().c_str(), device_id_);
            return true;
        }

        if( !engine_linker_ ) {
            MLOG_ERROR("EngineLinker not set for engine '%s' on device %d. Cannot activate.", profile_.GetProfileName().c_str(), device_id_);
            return false;
        }

        if( this->Link() == false ) {
            MLOG_ERROR("Try EngineLink, but failed at engine '%s' on device %d.", profile_.GetProfileName().c_str(), device_id_ );
            return false;
        }

        // 하위 클래스의 모델 로드 및 버퍼 할당 로직 호출
        if( InitializeDeviceAndModel() == false ) {
            MLOG_ERROR("Failed to initialize model and buffers for engine '%s' on device %d.", profile_.GetProfileName().c_str(), device_id_);
            return false;
        }

        // 실행 상태 설정 및 추론 스레드 시작
        this->inference_thread_.SetThreadFunctions(
            // runner
            std::bind( &EngineHandlerBase::InferenceThreadRunner, this ),
            // closer
            std::bind( &EngineHandlerBase::InferenceThreadCloser, this )
        );
        this->inference_thread_.Start();

        MLOG_DEBUG("Engine '%s' on device %d activated successfully.", profile_.GetProfileName().c_str(), device_id_);
        is_activate_handler_.store( true );
        return true;
    }

    void EngineHandlerBase::TerminateEngine( void )
    {
        // 이미 종료되었거나 종료 중이면 반환 (exchange 사용)
        if( is_activate_handler_.exchange(false) == false ) {
            return;
        }
        MLOG_DEBUG("Terminating engine '%s' on device %d...", profile_.GetProfileName().c_str(), device_id_);

        // 1. 입력 큐 종료 <- Closer에서 처리함
        // 2. 추론 스레드 종료 대기
        this->inference_thread_.Stop();

        // 3. 하위 클래스의 장치 자원 해제 호출
        ReleaseDeviceResources();
        MLOG_DEBUG("Device resources released for engine '%s', device %d.", profile_.GetProfileName().c_str(), device_id_);

        // 4. 큐 포인터 리셋 (shared_ptr 자동 관리되지만 명시적 해제)
        request_q_.reset();
        respond_q_.reset();
        engine_linker_ = nullptr; // 링커 함수 포인터도 클리어

        MLOG_DEBUG("   - Engine '%s' on device %d terminated successfully.", profile_.GetProfileName().c_str(), device_id_);
    }

    void EngineHandlerBase::SetEngineLinker( const EngineLinker& linker )
    {
        this->engine_linker_ = linker;
    }

    bool EngineHandlerBase::Link( void )
    {
        if( !this->engine_linker_ ) {
            MLOG_ERROR("Engine '%s' on device %d Try link to EngineLoadBalancer, but failed",
                profile_.GetProfileName().c_str(), device_id_);
            return false;
        }

        request_q_ = std::make_shared<SafeQueue<InputLayerWrapper>>();
        if( !request_q_ ){
            MLOG_ERROR("Failed to create input queue for engine '%s', device %d.", profile_.GetProfileName().c_str(), device_id_);
            engine_linker_ = nullptr; // 링커 설정 원복
            return false;
        }

        // LoadBalancer의 Linker 함수 호출 (handle_uuid, input_queue 전달 -> output_queue 받기)
        try {
            respond_q_ = engine_linker_( GetEngineHandleUUID(), request_q_ );
        } catch (const std::exception& e) {
            MLOG_ERROR("Exception calling EngineLinker for engine '%s', device %d: %s",
                profile_.GetProfileName().c_str(), device_id_, e.what());
            respond_q_ = nullptr; // 예외 발생 시 null로 설정
        } catch (...) {
            MLOG_ERROR("Unknown exception calling EngineLinker for engine '%s', device %d.",
                profile_.GetProfileName().c_str(), device_id_);
            respond_q_ = nullptr;
        }

        // 결과 확인
        if (!respond_q_) {
            MLOG_ERROR("Failed to get output queue via EngineLinker for engine '%s', device %d.",
                profile_.GetProfileName().c_str(), device_id_);
            request_q_.reset();    // 생성한 입력 큐 해제
            engine_linker_ = nullptr; // 링커 설정 원복
            return false;
        }

        MLOG_DEBUG("EngineLinker set successfully for engine '%s', device %d.", profile_.GetProfileName().c_str(), device_id_);
        return true;
    }

    bool EngineHandlerBase::IsActive(void) const noexcept
    {
        // atomic 변수의 현재 값 로드하여 반환
        return is_activate_handler_.load( std::memory_order_relaxed );
    }

    bool EngineHandlerBase::InitializeDeviceAndModel( void )
    {
        // LoadModelEngineFile, AllocateBuffers 호출 (순서는 구현에 따라 조정)
        if( LoadModelEngineFile() == false ){
            MLOG_ERROR("LoadModelEngineFile() failed for engine '%s', device %d.",
                profile_.GetProfileName().c_str(), device_id_);
            return false;
        }

        if( AllocateBuffers() == false ){
            MLOG_ERROR("AllocateBuffers failed for engine '%s', device %d.",
                profile_.GetProfileName().c_str(), device_id_);

            // 모델 로드 후 버퍼 할당 실패 시, 로드된 모델 자원 해제 로직 필요할 수 있음
            // ReleaseDeviceResources(); // <- 여기서 호출하면 안 됨 (TerminateEngine에서 호출)
            // 별도의 모델 자원 해제 함수 호출 필요 시 고려
            return false;
        }
        return true;
    }

    OutputLayerWrapper EngineHandlerBase::BuildInferenceFailureRespond( const InputLayerWrapper& input_layer )
    {
        std::vector<InferObject> empty_vector {};
        empty_vector.clear();

        return OutputLayerWrapper::Build( input_layer.meta_data, this->GetEngineHandleUUID(), std::move( empty_vector ) );
    }

    void EngineHandlerBase::InferenceThreadRunner( void )
    {
        MLOG_DEBUG("[Thread:%#llx] Inference loop started for engine '%s', device %d.",
            std::this_thread::get_id(), profile_.GetProfileName().c_str(), device_id_);

        auto& is_running = this->inference_thread_.GetRunningFlag();

        // 스레드 내 장치 초기화 (매우 중요! CUDA 컨텍스트는 스레드별로 필요할 수 있음)
        if( InitializeDevice() == false ) {
            MLOG_ERROR("[Thread:%#llx] InitializeDevice failed in inference loop for engine '%s', device %d. Terminating loop.",
                std::this_thread::get_id(), profile_.GetProfileName().c_str(), device_id_);
            is_running.store(false); // 루프 즉시 종료 플래그 설정
        }

        using namespace std::chrono_literals;
        while( is_running.load() == true ) {

            // dequeue_wait_for: 종료/타임아웃 시 std::nullopt 반환 (throw 없음).
            // SafeQueue::terminate() 호출 시 즉시 nullopt → 다음 loop 에서 is_running 검사로 종료.
            auto opt_input = request_q_->dequeue_wait_for( 100ms );
            if( !opt_input.has_value() ) {
                continue;
            }
            InputLayerWrapper input_data = std::move( *opt_input );

            // Preprocess 호출 (하위 클래스 구현)
            bool   batch_ready = false;
            size_t prev_batched_input_size = current_batch_inputs_.size();

            try {
                batch_ready = Preprocess( input_data );
            }
            catch (const std::exception& e) {
                MLOG_ERROR("[Thread:%#llx] Exception during Preprocess for engine '%s', device %d: %s.",
                    std::this_thread::get_id(), profile_.GetProfileName().c_str(), device_id_, e.what());

                // 만약 Preprocess 과정에서 prev_batched_input_size 가 증가되었다?
                // 이미 NPU 처리 device에 메모리 올라간 상태이므로, 후처리 없이 정상 플로우로 흘려보내야 함.
                // 그러면 inference 결과가 나오므로 그것을 반환하면 됨.
                //
                // 반대로 current_batch_inputs_.size() 가 동일하다면, NPU에 입력조차 되지 않은 상황이므로
                // Inference requester 가 무한 대기 하지 않도록 실패 처리된 OutputLayerWrapper 반환.
                if( prev_batched_input_size == current_batch_inputs_.size() ) {
                    respond_q_->enqueue_move( std::move( this->BuildInferenceFailureRespond( input_data ) ) );
                }
                continue;
            }
            catch (...) {
                MLOG_ERROR("[Thread:%#llx] Unknown exception during Preprocess for engine '%s', device %d. Skipping item.",
                    std::this_thread::get_id(), profile_.GetProfileName().c_str(), device_id_);

                if( prev_batched_input_size == current_batch_inputs_.size() ) {
                    respond_q_->enqueue_move( std::move( this->BuildInferenceFailureRespond( input_data ) ) );
                }
                continue;
            }

            // 배치가 준비되지 않았으면 다음 입력 대기
            if( batch_ready == false ) {
                continue;
            }

            // DoInference 호출 (하위 클래스 구현)
            bool infer_success = false;
            try {
                infer_success = DoInference();
            }
            catch (const std::exception& e) {
                MLOG_ERROR("[Thread:%#llx] Exception during DoInference for engine '%s', device %d: %s.",
                    std::this_thread::get_id(), profile_.GetProfileName().c_str(), device_id_, e.what());
            }
            catch (...) {
                MLOG_ERROR("[Thread:%#llx] Unknown exception during DoInference for engine '%s', device %d.",
                    std::this_thread::get_id(), profile_.GetProfileName().c_str(), device_id_);
            }

            if( infer_success == false ){
                MLOG_ERROR("[Thread:%#llx] DoInference failed for engine '%s', device %d. Skipping postprocess.",
                    std::this_thread::get_id(), profile_.GetProfileName().c_str(), device_id_);

                // 추론 실패 시 배치 관련 데이터 정리
                for( const auto& batch_input : current_batch_inputs_ ) {
                    respond_q_->enqueue_move( std::move( this->BuildInferenceFailureRespond( batch_input ) ) );
                }
                current_batch_inputs_.clear();
                continue; // 다음 입력 처리
            }

            // Postprocess 호출 (하위 클래스 구현)
            std::vector<OutputLayerWrapper> output_results;
            bool post_process_success = false;
            try {
                output_results = Postprocess();
                post_process_success = true;
            }
            catch (const std::exception& e) {
                MLOG_ERROR("[Thread:%#llx] Exception during Postprocess for engine '%s', device %d: %s.",
                    std::this_thread::get_id(), profile_.GetProfileName().c_str(), device_id_, e.what());
            }
            catch (...) {
                MLOG_ERROR("[Thread:%#llx] Unknown exception during Postprocess for engine '%s', device %d.",
                    std::this_thread::get_id(), profile_.GetProfileName().c_str(), device_id_);
            }

            if( post_process_success == false ){
                MLOG_ERROR("[Thread:%#llx] Postprocess failed for engine '%s', device %d.",
                    std::this_thread::get_id(), profile_.GetProfileName().c_str(), device_id_);

                // 추론 실패 시 배치 관련 데이터 정리
                for( const auto& batch_input : current_batch_inputs_ ) {
                    respond_q_->enqueue_move( std::move( this->BuildInferenceFailureRespond( batch_input ) ) );
                }
                current_batch_inputs_.clear();
                continue; // 다음 입력 처리
            }

            // 추론 자체의 결과는 객체 탐지 등이 되지 않더라도 무조건 OutputLayerWrapper는 반환해야 함
            if( output_results.size() != current_batch_inputs_.size() ) {
                MLOG_ERROR("output_results.size() [%d] != current_batch_inputs_.size() [%d]",
                    output_results.size(), current_batch_inputs_.size() );
                // ...
            }

            // 생성된 결과들을 출력 큐로 전송
            for( auto& result : output_results ) {
                respond_q_->enqueue_move( std::move( result ) );
            }
            // 처리 완료된 배치 관련 데이터 정리
            current_batch_inputs_.clear();
        }
    }

    void EngineHandlerBase::InferenceThreadCloser( void )
    {
        if( request_q_ )
        {
            request_q_->terminate();
            request_q_->clear_with_action( [this]( const InputLayerWrapper& in ) {
                if( this->respond_q_ ) {
                    respond_q_->enqueue_move( std::move( this->BuildInferenceFailureRespond( in ) ) );
                }
            });

            const int remains = static_cast<int>( request_q_->size() );
            if( remains > 0 ) {
                MLOG_INFO( "   - Engine[%s|%d] Request Queue Flushed ( remains : %d )",
                    profile_.GetProfileName().c_str(), device_id_, remains );
                std::this_thread::sleep_for( std::chrono::milliseconds( 100 ) );
            }
        }
    }

} // namespace MGEN
