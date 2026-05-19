#pragma once

#include "EngineStreamTypes.h"
#include "ReplyDispatcherWithCleaner.h"
#include "EngineProfile.h"
#include "InferenceCounter.h"

#include <atomic>
#include <map>
#include <unordered_map>
#include <vector>
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
     * @brief Multi-handler NPU 엔진 환경에서 추론 요청 분배 및 응답 디스패처를 담당하는 클래스.
     *
     * Odroid M2 NPU (RK3588, 3 core) + YoloV5 구성:
     *   - Linker 가 N 회 호출되어 handler N 개 등록 (3 handler = 3 NPU core 분산)
     *   - RequestAsync 가 round-robin 으로 handler 선택 + 해당 input_q enqueue (backpressure 체크)
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

        /**
         * @brief Inspection: 응답 receiver queue 의 현재 size (leak hunt 용).
         */
        size_t GetRespondReceiverSize() const noexcept {
            return infer_respond_receiver_ ? infer_respond_receiver_->size() : 0;
        }

        /**
         * @brief Inspection: 등록된 handler 의 input queue 의 합산 size (leak hunt 용).
         */
        size_t GetTotalInputQueueSize() const noexcept {
            std::lock_guard<std::mutex> lck { this->engine_mutex_ };
            size_t total = 0;
            for( const auto& [uuid, q] : this->engine_input_qs_ ) {
                (void) uuid;
                if( q ) total += q->size();
            }
            return total;
        }

        /**
         * @brief Inspection: ReplyDispatcher pending entry 합산 (leak hunt 용 — set_reply 후 wait_and_get 으로 erase 안 된 entry 누적 감시).
         *        B4 (cam-별 result queue) 도입 후 reply_dispatcher_ 가 미사용 — 항상 0 반환.
         */
        size_t GetReplyDispatcherEntrySize() const noexcept {
            return reply_dispatcher_.total_size();
        }

        /**
         * @brief Inspection: cam-별 result queue 의 합산 size (B4 추가).
         *        NPU 결과가 RspThread 가 pop 하기 전 까지 보유. 평균 0~1 기대 (RspThread drain rate >> NPU output rate).
         */
        size_t GetCamResultQTotalSize() const noexcept {
            std::lock_guard<std::mutex> lck { this->cam_result_qs_mutex_ };
            size_t total = 0;
            for( const auto& [uuid, q] : this->cam_result_qs_ ) {
                (void) uuid;
                if( q ) total += q->size();
            }
            return total;
        }

        /**
         * @brief Inspection: cam-별 result queue 의 drop_oldest 누적 카운터 합산 (B4 추가).
         */
        uint64_t GetCamResultQTotalDrop() const noexcept {
            std::lock_guard<std::mutex> lck { this->cam_result_qs_mutex_ };
            uint64_t total = 0;
            for( const auto& [uuid, q] : this->cam_result_qs_ ) {
                (void) uuid;
                if( q ) total += q->GetDropCount();
            }
            return total;
        }

    private:
        void ReplyDispatcherThreadRunner( void );
        void ReplyDispatcherThreadCloser( void );

    private:
        // 로드 가능한 엔진 프로필 정보 (불변, 락 불필요)
        const std::unordered_map<InferEngineID, EngineProfile> engine_profiles_;

        // Multi-handler — Linker 가 N 회 호출되어 누적. RequestAsync 가 round-robin 선택.
        std::vector<EngineHandleUUID>                                       engine_handles_;
        std::map<EngineHandleUUID, sptrSafeQueue<InputLayerWrapper>>        engine_input_qs_;
        std::atomic<size_t>                                                 round_robin_idx_ { 0 };

        // 응답 수신 큐 (모든 응답이 모이는 단일 큐 — NPU handler 들이 push, ReplyDispatcherThread 가 dequeue)
        sptrSafeQueue<OutputLayerWrapper> infer_respond_receiver_;

        // (DEPRECATED B4 이후) Unit ID 기반 응답 분배기 — cam-별 entry 1개만 보유 (덮어쓰기) 라
        //   backlog 처리 불가. RspThread cycle = NPU pacing 으로 묶임.
        // B4 (2026-05-18) — cam_result_qs_ 로 교체. ReplyDispatcherThreadRunner 가 cam-별 큐 push.
        //   reply_dispatcher_ 자체는 호환 위해 남기지만 set_reply / wait_and_get 호출 없음.
        MGEN::ReplyDispatcherWithCleaner<OutputLayerWrapper, MGEN::Type::UnitID> reply_dispatcher_;

        // B4 — cam-별 result queue. NPU 결과 (cam 별) 가 도착 순서대로 push, RspThread 가 dequeue.
        //   기존 reply_dispatcher_ (entry 1개 덮어쓰기) 의 backlog drain 불가 문제 해결.
        //   cap = 10 (RspThread 가 빠르게 drain 하므로 cap 도달 거의 없음, 안전 buffer).
        std::map<MGEN::Type::UnitID, std::shared_ptr<SafeQueue<OutputLayerWrapper>>> cam_result_qs_;
        mutable std::mutex                                                            cam_result_qs_mutex_;

        // 결과 구독 유닛 목록
        std::set<MGEN::Type::UnitID> subscribers_;

        // 응답 처리 스레드
        MGEN::SafeThread reply_dispatcher_thread_;

        // 뮤텍스
        mutable std::mutex unit_regist_mutex_; // subscribers_ 보호
        mutable std::mutex engine_mutex_;      // engine_handles_ / engine_input_qs_ / round_robin_idx_ 보호

        // 추론 카운터 + 종료 플래그
        InferenceCounter  inference_counter_;
        std::atomic<bool> is_terminate_instance_;
    };

} // namespace MGEN
