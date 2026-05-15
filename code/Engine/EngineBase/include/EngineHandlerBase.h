#pragma once

#include "SafeQueue.h"
#include "EngineStreamTypes.h"  // EngineLinker, EngineHandleUUID, InputLayerWrapper, OutputLayerWrapper
#include "EngineProfile.h"
#include "InferObject.h"
#include "SafeThread.h"

#include <atomic>
#include <vector>
#include <memory>
#include <utility>

namespace MGEN
{
    /**
     * @class EngineHandlerBase
     * @brief 모든 엔진 핸들러의 기반이 되는 추상 클래스.
     *        공통 로직(프로필, 디바이스 ID, 스레드, 큐, 배치 관리)을 제공.
     */
    class EngineHandlerBase
    {
    public:
        EngineHandlerBase() = delete;

        /**
         * @brief 생성자
         * @param profile 엔진 프로필 (const 참조)
         * @param device_id 할당된 장치 ID
         */
        explicit EngineHandlerBase( const EngineProfile& profile, const InferDeviceID device_id ) noexcept;

        /**
         * @brief 가상 소멸자. 엔진이 활성 상태면 종료를 시도.
         */
        virtual ~EngineHandlerBase();

        // 복사/이동 금지
        EngineHandlerBase( const EngineHandlerBase& ) = delete;
        EngineHandlerBase& operator=( const EngineHandlerBase& ) = delete;
        EngineHandlerBase( EngineHandlerBase&& ) = delete;
        EngineHandlerBase& operator=( EngineHandlerBase&& ) = delete;

        /**
         * @brief 엔진 활성화 (추론 스레드 시작)
         */
        virtual bool ActivateEngine( void );

        /**
         * @brief TerminateEngine = 영구 종료. 추론 thread Stop + 장치 자원 (RKNN ctx, 버퍼) 해제.
         *        호출 후 ActivateEngine() 으로 재활성화 불가. is_activate_handler_ atomic 으로 idempotent 보장.
         */
        virtual void TerminateEngine( void );

        /**
         * @brief LoadBalancer와의 연결 설정
         */
        virtual void SetEngineLinker( const EngineLinker& linker );

        /**
         * @brief 엔진 활성화 상태 반환
         */
        bool IsActive( void ) const noexcept;

        // --- Getter (const 멤버 직접 접근) ---
        const EngineProfile& GetProfile( void ) const noexcept { return profile_; }
        InferDeviceID        GetDeviceID( void ) const noexcept { return device_id_; }
        EngineHandleUUID     GetEngineHandleUUID( void ) const noexcept
        {
            return std::make_pair( profile_.GetProfileUUID(), device_id_ );
        }

    // 하위 클래스에서 구현해야 할 순수 가상 함수들
    protected:
        /** @brief 추론 스레드 내에서 장치 초기화 (NPU 환경에서는 no-op) */
        virtual bool InitializeDevice( void ) = 0;

        /** @brief 모델 파일 로드 및 엔진/컨텍스트 생성 (ActivateEngine에서 호출) */
        virtual bool LoadModelEngineFile( void ) = 0;

        /** @brief 추론용 입출력 버퍼 할당 (ActivateEngine에서 호출) */
        virtual bool AllocateBuffers( void ) = 0;

        /** @brief 할당된 장치 자원(버퍼 등) 해제 (TerminateEngine에서 호출) */
        virtual void ReleaseDeviceResources( void ) = 0;

        /**
         * @brief 입력 데이터를 받아 내부 배치 버퍼에 추가하고 준비.
         * @param input 처리할 입력 데이터
         * @return 배치가 가득 차서 추론을 진행할 준비가 되었으면 true, 아니면 false.
         */
        virtual bool Preprocess( const InputLayerWrapper& input ) = 0;

        /**
         * @brief 준비된 배치 데이터로 실제 추론 실행.
         * @return 추론 성공 시 true, 실패 시 false.
         */
        virtual bool DoInference( void ) = 0;

        /**
         * @brief 추론 결과를 후처리하여 OutputLayerWrapper 객체들 생성.
         * @return 처리된 결과 객체들의 std::vector. 실패 시 빈 벡터 반환 가능.
         */
        virtual std::vector<OutputLayerWrapper> Postprocess( void ) = 0;

    // 공통 멤버 변수 ( 하위 클래스 접근 가능 )
    protected:
        const EngineProfile profile_;       // 엔진 프로필 (불변)
        const InferDeviceID device_id_;     // 할당된 장치 ID (불변)

        MGEN::SafeThread inference_thread_; // 추론 스레드 클래스
        unsigned int     batch_size_;       // 엔진 배치 크기 ( from EngineProfile )

        EngineLinker engine_linker_ = nullptr; // EngineLoadBalancer에서 할당받은 linker 함수
        sptrSafeQueue<InputLayerWrapper>  request_q_ = nullptr; // 입력 데이터 큐 ( to EngineLoadBalancer )
        sptrSafeQueue<OutputLayerWrapper> respond_q_ = nullptr; // 결과 데이터 큐 ( from EngineLoadBalancer )

        // 배치 입력 및 결과 후처리를 위한 데이터 저장소
        // InputLayerWrapper 전체를 저장하여 추론(비동기 메모리 복사 포함)이
        // 완료될 때까지 image_data(shared_ptr)의 생명주기를 보장합니다.
        std::vector<InputLayerWrapper> current_batch_inputs_;

        // EngineHandler 자체의 생명주기 나타내는 atomic boolean
        std::atomic<bool> is_activate_handler_ { false };

    // 내부 헬퍼 함수
    protected:
        /**
         * @brief LoadBalancer와의 연결 실행
         * @return 성공 시 true, 실패 시 false
         */
        bool Link( void );

        /**
         * @brief 모델 로드 및 버퍼 할당 수행 (ActivateEngine 내부 호출용)
         * @return 성공 시 true, 실패 시 false
         */
        bool InitializeDeviceAndModel( void );

        /**
         * @brief 추론 스레드 메인 루프 Runner 함수. 해당 함수를 SafeThread에 등록해서 사용.
         */
        void InferenceThreadRunner( void );

        /**
         * @brief 추론 스레드 메인 루프 Closer 함수. 해당 함수를 SafeThread에 등록해서 사용.
         */
        void InferenceThreadCloser( void );

        /**
         * @brief 내부 추론 과정에서 실패 시 요청했던 InputLyaerWrapper를 참조해서 오류 반환값을 생성
         * @param input_layer 요청했던 InputLayerWrapper
         * @return InferObject 없는 emtpy vector 포함한 반환물
         */
        OutputLayerWrapper BuildInferenceFailureRespond( const InputLayerWrapper& input_layer );
    };

} // namespace MGEN
