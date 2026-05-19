#pragma once

#include "EngineHandlerBase.h"
#include "InferObject.h"

// STL
#include <vector>
#include <string>
#include <memory>
#include <mutex>

// OpenCV
#include <opencv2/opencv.hpp>
#include <opencv2/imgproc/types_c.h>

// RKNN
#include "rknn_api.h"

namespace MGEN
{
    namespace YOLOV5_RKNN
    {
        constexpr size_t INPUT_LAYER_INDEX  = 0;
        constexpr size_t OUTPUT_LAYER_INDEX = 1;

        constexpr size_t IN_LAYER_BATCH_SIZE_INDEX = 0;
        constexpr size_t IN_LAYER_CHANNEL_INDEX    = 1;
        constexpr size_t IN_LAYER_HEIGHT_INDEX     = 2;
        constexpr size_t IN_LAYER_WIDTH_INDEX      = 3;

        constexpr size_t OUT_LAYER_BATCH_SIZE_INDEX = 0;
        constexpr size_t OUT_LAYER_NUM_BOXES_INDEX  = 1;
        constexpr size_t OUT_LAYER_INFER_DATA_INDEX = 2;

        constexpr size_t BBOX_ELEMENT_COUNT = 4;
        constexpr size_t CONF_ELEMENT_COUNT = 1;

        constexpr size_t INPUT_H   = 640;
        constexpr size_t INPUT_W   = 640;
        constexpr size_t LOCATIONS = 4;

        struct alignas(float) Detection
        {
            float bbox[LOCATIONS]; // ltx lty w h
            float conf;            // bbox_conf * cls_conf
            float class_id;
        };
    } // namespace YOLOV5_RKNN

    struct rknn_app_context_t
    {
        rknn_input_output_num io_num;
        rknn_tensor_attr*     input_attrs;
        rknn_tensor_attr*     output_attrs;

        rknn_app_context_t()
            : io_num()
            , input_attrs( nullptr )
            , output_attrs( nullptr )
        {}
    };

    /**
     * @class YoloV5_Torch_Onnx_RKNN_NPU
     * @brief YoloV5 모델을 RKNN으로 최적화하여 NPU에서 실행하는 EngineHandler 구현체.
     * EngineHandlerBase를 상속받아 NPU/RKNN 관련 로직을 구체화합니다.
     */
    class YoloV5_Torch_Onnx_RKNN_NPU : public EngineHandlerBase
    {
    public:
        /**
         * @param profile   엔진 프로필 (const 참조).
         * @param device_id NPU 코어 ID (0/1/2). LoadEngine 시 rknn_set_core_mask(CORE_<id>) 호출.
         *                  Multi-handler 환경 (3 handler × 3 NPU core) 에서 각 handler 가
         *                  서로 다른 core 를 고정 사용 → 3-way parallel inference.
         */
        explicit YoloV5_Torch_Onnx_RKNN_NPU( const EngineProfile& profile, const InferDeviceID device_id ) noexcept;

        /**
         * @brief 소멸자 (자원 해제는 TerminateEngine -> ReleaseDeviceResources 에서 처리)
         */
        ~YoloV5_Torch_Onnx_RKNN_NPU() override = default;

        // 복사 및 이동 생성자/대입 연산자 삭제 (Rule of Five/Zero)
        YoloV5_Torch_Onnx_RKNN_NPU( const YoloV5_Torch_Onnx_RKNN_NPU& ) = delete;
        YoloV5_Torch_Onnx_RKNN_NPU& operator=( const YoloV5_Torch_Onnx_RKNN_NPU& ) = delete;
        YoloV5_Torch_Onnx_RKNN_NPU( YoloV5_Torch_Onnx_RKNN_NPU&& ) = delete;
        YoloV5_Torch_Onnx_RKNN_NPU& operator=( YoloV5_Torch_Onnx_RKNN_NPU&& ) = delete;

    // EngineHandlerBase의 순수 가상 함수 구현
    protected:
        /**
         * @brief 추론 스레드 컨텍스트에서 사용할 NPU 장치를 설정합니다. GPU 호환을 위한 항등함수.
         * @return 성공 시 true, 실패 시 false. true 고정
         */
        virtual bool InitializeDevice( void ) override final;

        /**
         * @brief 프로필에 명시된 경로에서 rknn 엔진 파일을 로드합니다.
         * @return 성공 시 true, 실패 시 false.
         */
        virtual bool LoadModelEngineFile( void ) override final;

        /**
         * @brief 로드된 rknn 엔진의 바인딩 정보를 분석하여 필요한 NPU 메모리 버퍼들을 할당합니다.
         * @return 성공 시 true, 실패 시 false.
         */
        virtual bool AllocateBuffers( void ) override final;

        /**
         * @brief AllocateBuffers에서 할당했던 모든 NPU/Host 메모리 버퍼를 해제합니다
         */
        virtual void ReleaseDeviceResources( void ) override final;

        /**
         * @brief 입력 데이터를 받아 전처리 및 NPU 입력 버퍼 배치를 수행합니다.
         * @param input 처리할 입력 데이터 (InputLayerWrapper const 참조).
         * @return 현재까지 모인 입력 데이터 수가 배치 크기와 같거나 커지면 true를 반환합니다.
         */
        virtual bool Preprocess( const InputLayerWrapper& input ) override final;

        /**
         * @brief 준비된 배치 데이터로 RKNN 추론을 실행합니다 (비동기).
         * @return 작업 제출(enqueue 및 memcpy 시작) 성공 시 true, 실패 시 false.
         * @note 이 함수는 스트림 동기화를 수행하지 않습니다.
         */
        virtual bool DoInference( void ) override final;

        /**
         * @brief 추론 결과를 후처리하여 최종 결과 객체를 생성합니다.
         * @return 처리된 결과 객체들의 std::vector<OutputLayerWrapper>. 벡터 크기는 처리된 입력 수와 동일.
         */
        virtual std::vector<OutputLayerWrapper> Postprocess( void ) override final;

    // members
    private:
        // engine handler implements class name
        const std::string handler_name_;

        // const value
        const float confidence_threshold_;
        const float nms_threshold_;

        // value from engine context
        size_t class_num_ = 0;
        size_t output_index_num_ = 0;

        // Inference
        rknn_app_context_t      rknn_app_ctx_;
        rknn_context            rknn_ctx_ = 0;
        std::vector<rknn_input> rknn_inputs_;
        rknn_output*            rknn_outputs_ = nullptr;
        // NEW-6: rknn_outputs_byte_size_ dead member 제거 (사용처 0건).
        size_t                  rknn_model_c = 0;
        size_t                  rknn_model_w = 0;
        size_t                  rknn_model_h = 0;
        mutable std::mutex      rknn_input_lock_;

        cv::Rect GetRect( const cv::Mat& img, float bbox[4] );

        int PostProcessImplRKNN(
            int8_t* input_0, int8_t* input_1, int8_t* input_2, const int model_in_h, const int model_in_w,
            const std::vector<int32_t>& qnt_zps, const std::vector<float>& qnt_scales,
            std::vector<YOLOV5_RKNN::Detection>& results
        );
    };
}