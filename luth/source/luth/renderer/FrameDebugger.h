#pragma once

#include "luth/core/LuthTypes.h"
#include "luth/renderer/DrawCommand.h"
#include "luth/renderer/rendergraph/FrameCapture.h"
#include "luth/renderer/backend/vulkan/VulkanPipeline.h"

#include <vulkan/vulkan.h>
#include <memory>
#include <string>
#include <vector>

namespace Luth
{
    enum class DebuggerState : u8 { Inactive, CaptureRequested, Frozen };

    struct FrameDebugger
    {
        // Capture state machine
        DebuggerState     state             = DebuggerState::Inactive;
        RG::CapturedFrame capturedFrame;
        u32               drawLimit         = UINT32_MAX;
        u32               replayDrawCounter = 0;

        // Captured draw commands for re-recording (copied at freeze time)
        std::vector<DrawCommand> capturedOpaqueDraws;
        std::vector<DrawCommand> capturedCutoutDraws;
        std::vector<DrawCommand> capturedTransparentDraws;

        // Debug blit resources (rescue blit for truncated frames)
        std::unique_ptr<VKPipeline>  blitPipeline;
        std::unique_ptr<VKPipeline>  depthPipeline;
        std::vector<u32>             blitFragSpv;
        std::vector<u32>             depthFragSpv;
        VkDescriptorPool             descPool      = VK_NULL_HANDLE;
        VkDescriptorSetLayout        descSetLayout = VK_NULL_HANDLE;
        VkDescriptorSet              descSet       = VK_NULL_HANDLE;
        VkSampler                    sampler       = VK_NULL_HANDLE;

        // Capture helpers (called during normal recording when CaptureRequested)
        void BeginCapturePass(const std::string& name, const std::string& activeTarget,
                              bool isDepth, const RG::CapturedPipelineState& ps);
        void EndCapturePass();
        void CaptureDrawCall(const std::string& passName, const std::string& meshName,
                             const std::string& entityName, u32 entityIndex, u32 indexCount,
                             const ObjectPushConstants& pc, const RG::CapturedPipelineState& ps);

        void Shutdown(VkDevice device);
    };
}
