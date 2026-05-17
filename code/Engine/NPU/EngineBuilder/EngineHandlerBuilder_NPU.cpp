#include "EngineHandlerBuilder_NPU.h"
#include "MgenLogger.h"

#include "YoloV5_Torch_Onnx_RKNN_NPU.h"

// 표준 라이브러리
#include <cstdlib>

namespace MGEN
{
    std::vector<InferDeviceID> EngineHandlerBuilder_NPU::GetAvailableDeviceIDs() const
    {
        // RK3588 NPU = 3 physical cores (Core 0/1/2). 각 core 를 가상 device 1개로 노출.
        //   handler N 이 device N 으로 만들어지면 rknn_set_core_mask(CORE_N) 으로 고정 →
        //   librknnrt 의 단일 ctx serialize 제약 회피, 3-way parallel inference.
        std::vector<InferDeviceID> npu_ids;
        npu_ids.push_back( static_cast<InferDeviceID>( 0 ) );
        npu_ids.push_back( static_cast<InferDeviceID>( 1 ) );
        npu_ids.push_back( static_cast<InferDeviceID>( 2 ) );
        return npu_ids;
    }

    bool EngineHandlerBuilder_NPU::IsProfileSupported(const EngineProfile& profile) const
    {
        // NPU 환경 — Torch_Onnx_RKNN 만 지원
        if( profile.GetModelOptimizeType() != ModelOptimizeType::Torch_Onnx_RKNN ){
            MLOG_WARN("Profile '%s' uses optimize type (%d) not suitable for NPU builder.",
                profile.GetProfileName().c_str(), static_cast<int>(profile.GetModelOptimizeType()));
            return false;
        }
        return true;
    }

    std::vector<std::unique_ptr<EngineHandlerBase>> EngineHandlerBuilder_NPU::BuildHandlers(
        const std::vector<EngineProfile>& profiles_to_build,
        const EngineLinker& linker )
    {
        std::vector<std::unique_ptr<EngineHandlerBase>> created_handlers;

        // RK3588 NPU 3 코어 = handler 3개 (각 코어 fix). profile 마다 N 코어 만큼 생성.
        const auto npu_ids = GetAvailableDeviceIDs();

        for( const auto& profile : profiles_to_build )
        {
            if( !IsProfileSupported(profile) ){
                MLOG_WARN("Profile '%s' is not supported by NPU builder. Skipping.", profile.GetProfileName().c_str());
                continue;
            }

            for( const InferDeviceID npu_id : npu_ids ) {
                MLOG_INFO(" -> Assigning profile '%s' to NPU device %d",
                    profile.GetProfileName().c_str(), npu_id);

                auto handler = CreateAndLinkHandler( profile, npu_id, linker );
                if( handler ){
                    created_handlers.push_back( std::move( handler ) );
                } else {
                    MLOG_ERROR(" -> Failed to create handler for profile '%s' on NPU %d.",
                        profile.GetProfileName().c_str(), npu_id);
                }
            }
        }

        MLOG_INFO("Finished building NPU handlers. Total created: %zu", created_handlers.size());
        return created_handlers;
    }

    // --- Private Helper Methods ---

    // 핸들러 생성 및 링커 설정을 묶는 헬퍼 함수
    std::unique_ptr<EngineHandlerBase> EngineHandlerBuilder_NPU::CreateAndLinkHandler(
        const EngineProfile& profile, InferDeviceID device_id, const EngineLinker& linker )
    {
        // 1. 실제 핸들러 객체 생성 (내부 팩토리 호출)
        std::unique_ptr<EngineHandlerBase> handler_base = CreateNpuEngineHandler( profile, device_id );

        if( handler_base == nullptr ){
            MLOG_ERROR("CreateNpuEngineHandler returned nullptr for profile '%s', device %d.",
                profile.GetProfileName().c_str(), device_id);
            return nullptr; // 핸들러 생성 실패
        }

        // 2. EngineLinker 설정
        handler_base->SetEngineLinker( linker );

        MLOG_DEBUG("Handler created and linker set for profile '%s' on device %d.",
            profile.GetProfileName().c_str(), device_id);

        return handler_base;
    }

    // 특정 프로필에 맞는 NPU 핸들러 구현체 생성 (내부 팩토리)
    std::unique_ptr<EngineHandlerBase> EngineHandlerBuilder_NPU::CreateNpuEngineHandler(
        const EngineProfile& profile, InferDeviceID assigned_device_id )
    {
        /*** 이 부분을 실제 프로젝트의 핸들러 클래스 구조에 맞게 수정해야 함 ***/
        ModelMajorType    major_type = profile.GetModelMajorType();
        ModelMinorType    minor_type = profile.GetModelMinorType();
        ModelMagicType    magic_type = profile.GetModelMagicType();
        ModelOptimizeType optmz_type = profile.GetModelOptimizeType();

        if( major_type == ModelMajorType::Detection )
        {
            if( minor_type == ModelMinorType::YoloV5 ) {
                MLOG_INFO("Instantiating RKNN for profile '%s' on device %d.",
                    profile.GetProfileName().c_str(), assigned_device_id);

                return std::make_unique<YoloV5_Torch_Onnx_RKNN_NPU>( profile, assigned_device_id );
            }
            // else if ...
            else {
                MLOG_ERROR("Profile '%s' ( MajorType '%s', MinorType '%s', Optimized '%s' ) is not surpported current.",
                    profile.GetProfileName().c_str(),
                    EngineProfile::ToString( major_type ).c_str(),
                    EngineProfile::ToString( minor_type ).c_str(),
                    EngineProfile::ToString( optmz_type ).c_str());
                return nullptr;
            }

        }
        else {
            MLOG_ERROR("Profile '%s' ( MajorType '%s', MinorType '%s', Optimized '%s' ) is not surpported current.",
                profile.GetProfileName().c_str(),
                EngineProfile::ToString( major_type ).c_str(),
                EngineProfile::ToString( minor_type ).c_str(),
                EngineProfile::ToString( optmz_type ).c_str());
            return nullptr;
        }
    }

} // namespace MGEN