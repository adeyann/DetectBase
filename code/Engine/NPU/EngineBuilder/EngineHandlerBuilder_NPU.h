#pragma once

#include "EngineHandlerBase.h"
#include "EngineProfile.h"
#include "EngineStreamTypes.h"

#include <vector>
#include <memory>
#include <string>

namespace MGEN
{
    /**
     * @class EngineHandlerBuilder_NPU
     * @brief NPU 환경 (RK3588) 엔진 핸들러 빌더.
     */
    class EngineHandlerBuilder_NPU
    {
    public:
        EngineHandlerBuilder_NPU( void ) = default;
        ~EngineHandlerBuilder_NPU() = default;

        EngineHandlerBuilder_NPU( const EngineHandlerBuilder_NPU& ) = delete;
        EngineHandlerBuilder_NPU& operator=( const EngineHandlerBuilder_NPU& ) = delete;
        EngineHandlerBuilder_NPU( EngineHandlerBuilder_NPU&& ) = delete;
        EngineHandlerBuilder_NPU& operator=( EngineHandlerBuilder_NPU&& ) = delete;

        /**
         * @brief NPU 프로필 목록과 링커를 기반으로 엔진 핸들러 인스턴스들을 생성.
         */
        std::vector<std::unique_ptr<EngineHandlerBase>> BuildHandlers(
            const std::vector<EngineProfile>& profiles_to_build,
            const EngineLinker& linker
        );

        /**
         * @brief 시스템에서 사용 가능한 NPU 장치 ID 목록 반환 (단일 NPU id=0).
         */
        std::vector<InferDeviceID> GetAvailableDeviceIDs( void ) const;

        /**
         * @brief 특정 프로필이 NPU 빌더에서 지원되는지 확인 (Torch_Onnx_RKNN).
         */
        bool IsProfileSupported( const EngineProfile& profile ) const;

    private:
        /**
         * @brief 특정 프로필과 할당된 NPU ID에 맞는 구체적인 NPU 핸들러 인스턴스 생성.
         */
        std::unique_ptr<EngineHandlerBase> CreateNpuEngineHandler(
            const EngineProfile& profile,
            InferDeviceID assigned_device_id
        );

        /**
         * @brief 핸들러 생성 + EngineLinker 설정 헬퍼.
         */
        std::unique_ptr<EngineHandlerBase> CreateAndLinkHandler(
            const EngineProfile& profile,
            InferDeviceID device_id,
            const EngineLinker& linker
        );
    };

} // namespace MGEN
