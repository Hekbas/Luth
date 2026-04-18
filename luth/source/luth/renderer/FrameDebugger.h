#pragma once

#include "luth/core/LuthTypes.h"
#include "luth/renderer/draw/DrawCommand.h"
#include "luth/renderer/rendergraph/FrameCapture.h"
#include "luth/renderer/rendergraph/IArchiveSink.h"
#include "luth/renderer/backend/vulkan/VulkanPipeline.h"

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

typedef struct VmaAllocator_T* VmaAllocator;

namespace Luth
{
    enum class DebuggerState : u8 { Inactive, CaptureRequested, Frozen };

    struct FrameDebugger : public RG::IArchiveSink
    {
        // Capture state machine
        DebuggerState     state = DebuggerState::Inactive;
        RG::CapturedFrame capturedFrame;

        // Phase 14C — drawLimit / replayDrawCounter / captured*Draws removed.
        // Live re-replay is gone; per-draw stepping (Phase 14E) reads frozen UBOs/
        // SSBOs/indirect and re-records the owning pass via ImmediateSubmit.

        // Debug blit resources (still used by Phase 14D for depth->color preview)
        std::unique_ptr<VKPipeline>  blitPipeline;
        std::unique_ptr<VKPipeline>  depthPipeline;
        std::vector<u32>             blitFragSpv;
        std::vector<u32>             depthFragSpv;
        VkDescriptorPool             descPool      = VK_NULL_HANDLE;
        VkDescriptorSetLayout        descSetLayout = VK_NULL_HANDLE;
        VkDescriptorSet              descSet       = VK_NULL_HANDLE;
        VkSampler                    sampler       = VK_NULL_HANDLE;

        // Phase 14B — Archive sink configuration. Names match RG::TextureDesc::name.
        // Set by RegisterTrackedRT before each capture.
        std::unordered_set<std::string> trackedRTs;

        // Cached device/allocator for archive ownership. Populated by BeginCapture.
        VkDevice     archiveDevice    = VK_NULL_HANDLE;
        VmaAllocator archiveAllocator = nullptr;

        // Capture helpers (called during normal recording when CaptureRequested)
        void BeginCapturePass(const std::string& name, const std::string& activeTarget,
                              bool isDepth, const RG::CapturedPipelineState& ps);
        void EndCapturePass();
        void CaptureDrawCall(const std::string& passName, const std::string& meshName,
                             const std::string& entityName, u32 entityIndex, u32 indexCount,
                             const ObjectPushConstants& pc, const RG::CapturedPipelineState& ps);

        void CaptureIndirectDraw(const std::string& passName, const std::string& meshName,
                                 const std::string& entityName, u32 entityIndex, u32 indexCount,
                                 u32 gpuObjectIndex, VkDeviceSize indirectOffset,
                                 const RG::CapturedPipelineState& ps);

        void CaptureComputeDispatch(const std::string& passName, const std::string& shaderName,
                                    u32 groupCountX, u32 groupCountY, u32 groupCountZ);

        // Phase 14B — Archive lifecycle (called from RenderingSystem around capture frame)
        void BeginCapture(VkDevice device, VmaAllocator allocator);
        void RegisterTrackedRT(const std::string& name);
        void FinalizeCapture(const glm::mat4& viewProj);
        void DestroyArchives();

        // IArchiveSink — invoked post-pass during RenderGraph::Execute
        void OnPassExecuted(u32 passIndex, RG::RenderGraph& graph, VkCommandBuffer primaryCmd) override;

        void Shutdown(VkDevice device);
    };
}
