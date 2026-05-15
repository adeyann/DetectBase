#pragma once

#include "EngineStreamTypes.h"
#include "ReplyDispatcherWithCleaner.h"
#include "EngineProfile.h"
#include "InferenceCounter.h"

#include <atomic>
#include <unordered_map>
#include <SafeQueue.h>
#include <SafeThread.h>
#include <optional>
#include <set>
#include <mutex>
#include <memory>

namespace MGEN
{
    namespace
    {
        // 엔진 입력 큐 최대 크기 (RTSP 실시간성 고려, 큐가 밀리면 요청 drop)
        const auto MAX_ENGINE_INFER_INPUT_QUEUE_SIZE = 128;
    }

    /**
     * @class EngineLoadBalancer
     * @brief 단일 NPU 엔진 환경에서 추론 요청 분배 및 응답 디스패처를 담당하는 클래스.
     *
     * Odroid M2 NPU 1개 + YoloV5 단일 엔진 구성:
     *   - Linker 가 1회 호출되어 단일 엔진 핸들러를 등록
     *   - RequestAsync 가 단일 큐로 요청 enqueue (backpressure 체크)
     *   - 응답은 단일 응답 큐 → ReplyDispatcher 통해 unit_id 별 분배
     */
    class EngineLoadBalancer
    {
    public:
        EngineLoadBalancer( void ) = delete;

        /**
         * @brief EngineLoadBalancer 생성자
         * @param profiles 로드 가능한 엔진 프로필 목록
         */
        explicit EngineLoadBalancer( const std::vector<EngineProfile>& profiles ) noexcept;

        ~EngineLoadBalancer();

        /**
         * @brief Terminate = 영구 종료. ReplyDispatcher / inference_counter / engine_input_q 모두 정리.
         *        호출 후 객체는 IsLoadEngine() == false 상태가 되며, 재시작 메서드 없음.
         *        is_terminate_instance_ atomic 으로 idempotent 보장.
         */
        bool Terminate( void );

        /**
         * @brief 엔진 핸들러가 Linker 통해 등록되었는지 확인
         */
        bool IsLoadEngine( void ) const noexcept;

        /**
         * @brief 요구사항을 기반으로 사용가능한 InferEngineID 하나 반환
         */
        InferEngineID GetAvailableInferEngineID( const InferRequestRequireOneType& require ) const noexcept;

        /**
         * @brief 엔진 ID 를 기반으로 EngineProfile 반환
         */
        std::optional<EngineProfile> GetEngineProfileTargetInferEngineID( const InferEngineID id ) const noexcept;

        /**
         * @brief 추론 결과 수신 유닛(클라이언트) 등록
         */
        bool Subscribe( const MGEN::Type::UnitID unit_id ) noexcept;

        /**
         * @brief 유닛 등록 해제
         */
        void Unsubscribe( const MGEN::Type::UnitID unit_id ) noexcept;

        /**
         * @brief 엔진 핸들러를 로드 밸런서에 연결 (Linker 함수)
         * @param handle_uuid 핸들러 UUID
         * @param input_q     핸들러 입력 큐
         * @return 응답 큐 (실패 시 nullptr)
         */
        sptrSafeQueue<OutputLayerWrapper> Linker( const EngineHandleUUID& handle_uuid, sptrSafeQueue<InputLayerWrapper> input_q );

        /**
         * @brief Linker 함수에 대한 std::function 반환
         */
        EngineLinker GetEngineLinker( void );

        /**
         * @brief 비동기 추론 요청
         */
        bool RequestAsync( InputLayerWrapper&& request );

        /**
         * @brief 비동기 추론 결과 수신 대기
         */
        std::optional<OutputLayerWrapper> RespondAsync( const MGEN::Type::UnitID unit_id, const int timeout_ms = 3000 );

        /**
         * @brief 동기 추론 (RequestAsync + RespondAsync)
         */
        std::optional<OutputLayerWrapper> Request( InputLayerWrapper&& request, const int timeout_ms = 3000 );

        /**
         * @brief 모든 구독자 등록 해제
         */
        void DestroySubscribersAll( void );

        /**
         * @brief 특정 유닛의 추론 카운트 증가
         */
        void AddInferCount( const MGEN::Type::UnitID id );

        void StartInferenceCounter( void );
        void StopInferenceCounter ( void );

    private:
        void ReplyDispatcherThreadRunner( void );
        void ReplyDispatcherThreadCloser( void );

    private:
        // 로드 가능한 엔진 프로필 정보 (불변, 락 불필요)
        const std::unordered_map<InferEngineID, EngineProfile> engine_profiles_;

        // 단일 엔진 정보 (Linker 가 1회 호출되어 등록)
        EngineHandleUUID                  engine_uuid_     { INFER_ENGINE_ID_NOT_SET, 0 };
        sptrSafeQueue<InputLayerWrapper>  engine_input_q_  = nullptr;
        bool                              engine_linked_   = false;

        // 응답 수신 큐 (모든 응답이 모이는 단일 큐)
        sptrSafeQueue<OutputLayerWrapper> infer_respond_receiver_;

        // Unit ID 기반 응답 분배기
        MGEN::ReplyDispatcherWithCleaner<OutputLayerWrapper, MGEN::Type::UnitID> reply_dispatcher_;

        // 결과 구독 유닛 목록
        std::set<MGEN::Type::UnitID> subscribers_;

        // 응답 처리 스레드
        MGEN::SafeThread reply_dispatcher_thread_;

        // 뮤텍스
        mutable std::mutex unit_regist_mutex_; // subscribers_ 보호
        mutable std::mutex engine_mutex_;      // engine_uuid_, engine_input_q_, engine_linked_ 보호

        // 추론 카운터 + 종료 플래그
        InferenceCounter  inference_counter_;
        std::atomic<bool> is_terminate_instance_;
    };

} // namespace MGEN
