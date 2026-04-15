#include "luthpch.h"
#include "luth/renderer/FrameDebugger.h"

namespace Luth
{
    void FrameDebugger::BeginCapturePass(const std::string& name, const std::string& activeTarget,
                                         bool isDepth, const RG::CapturedPipelineState& ps)
    {
        if (state != DebuggerState::CaptureRequested) return;

        RG::CapturedPass cp;
        cp.name               = name;
        cp.firstDrawIndex     = (u32)capturedFrame.drawCalls.size();
        cp.drawCallCount      = 0;
        cp.pipelineState      = ps;
        cp.activeRenderTarget = activeTarget;
        cp.isDepthTarget      = isDepth;
        capturedFrame.passes.push_back(std::move(cp));
    }

    void FrameDebugger::EndCapturePass()
    {
        if (state != DebuggerState::CaptureRequested) return;
        if (capturedFrame.passes.empty()) return;

        auto& cp = capturedFrame.passes.back();
        cp.drawCallCount = (u32)capturedFrame.drawCalls.size() - cp.firstDrawIndex;
    }

    void FrameDebugger::CaptureDrawCall(const std::string& passName, const std::string& meshName,
                                        const std::string& entityName, u32 entityIndex, u32 indexCount,
                                        const ObjectPushConstants& pc, const RG::CapturedPipelineState& ps)
    {
        if (state != DebuggerState::CaptureRequested) return;

        RG::CapturedDrawCall cdc;
        cdc.globalIndex    = (u32)capturedFrame.drawCalls.size();
        cdc.passIndex      = capturedFrame.passes.empty() ? 0 : (u32)(capturedFrame.passes.size() - 1);
        cdc.passLocalIndex = capturedFrame.passes.empty() ? 0
                           : (u32)(capturedFrame.drawCalls.size() - capturedFrame.passes.back().firstDrawIndex);
        cdc.passName       = passName;
        cdc.meshName       = meshName;
        cdc.entityName     = entityName;
        cdc.entityIndex    = entityIndex;
        cdc.indexCount     = indexCount;
        cdc.modelMatrix    = pc.modelMatrix;
        cdc.materialIndex  = pc.materialIndex;
        cdc.shadeMode      = pc.shadeMode;
        cdc.entityID       = pc.entityID;
        cdc.boneOffset     = pc.boneOffset;
        cdc.pipelineState  = ps;
        capturedFrame.drawCalls.push_back(std::move(cdc));
    }

    void FrameDebugger::CaptureIndirectDraw(const std::string& passName, const std::string& meshName,
                                            const std::string& entityName, u32 entityIndex, u32 indexCount,
                                            u32 gpuObjectIndex, VkDeviceSize indirectOffset,
                                            const RG::CapturedPipelineState& ps)
    {
        if (state != DebuggerState::CaptureRequested) return;

        RG::CapturedDrawCall cdc;
        cdc.globalIndex       = (u32)capturedFrame.drawCalls.size();
        cdc.passIndex         = capturedFrame.passes.empty() ? 0 : (u32)(capturedFrame.passes.size() - 1);
        cdc.passLocalIndex    = capturedFrame.passes.empty() ? 0
                              : (u32)(capturedFrame.drawCalls.size() - capturedFrame.passes.back().firstDrawIndex);
        cdc.passName          = passName;
        cdc.meshName          = meshName;
        cdc.entityName        = entityName;
        cdc.entityIndex       = entityIndex;
        cdc.indexCount        = indexCount;
        cdc.pipelineState     = ps;
        cdc.kind              = RG::DispatchKind::IndexedIndirect;
        cdc.gpuObjectIndex    = gpuObjectIndex;
        cdc.indirectOffset    = indirectOffset;
        cdc.indirectDrawCount = 1;
        cdc.indirectStride    = (u32)sizeof(VkDrawIndexedIndirectCommand);
        capturedFrame.drawCalls.push_back(std::move(cdc));
    }

    void FrameDebugger::CaptureComputeDispatch(const std::string& passName, const std::string& shaderName,
                                               u32 groupCountX, u32 groupCountY, u32 groupCountZ)
    {
        if (state != DebuggerState::CaptureRequested) return;

        RG::CapturedDrawCall cdc;
        cdc.globalIndex    = (u32)capturedFrame.drawCalls.size();
        cdc.passIndex      = capturedFrame.passes.empty() ? 0 : (u32)(capturedFrame.passes.size() - 1);
        cdc.passLocalIndex = capturedFrame.passes.empty() ? 0
                           : (u32)(capturedFrame.drawCalls.size() - capturedFrame.passes.back().firstDrawIndex);
        cdc.passName       = passName;
        cdc.meshName       = shaderName;   // reused as shader label
        cdc.entityName     = "Dispatch";
        cdc.kind           = RG::DispatchKind::Compute;
        cdc.groupCountX    = groupCountX;
        cdc.groupCountY    = groupCountY;
        cdc.groupCountZ    = groupCountZ;
        cdc.pipelineState.shaderName = shaderName;
        capturedFrame.drawCalls.push_back(std::move(cdc));
    }

    void FrameDebugger::Shutdown(VkDevice device)
    {
        if (sampler)
            vkDestroySampler(device, sampler, nullptr);
        if (descSetLayout)
            vkDestroyDescriptorSetLayout(device, descSetLayout, nullptr);
        if (descPool)
            vkDestroyDescriptorPool(device, descPool, nullptr);
        blitPipeline.reset();
        depthPipeline.reset();
    }
}
