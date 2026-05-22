#pragma once

#include "luth/core/types/LuthMath.h"
#include "luth/renderer/draw/DrawCommand.h"
#include "luth/renderer/rendergraph/FrameCapture.h"
#include "luth/renderer/rendergraph/IArchiveSink.h"
#include "luth/renderer/backend/vulkan/VulkanPipeline.h"

#include <vulkan/vulkan.h>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

typedef struct VmaAllocator_T* VmaAllocator;

namespace Luth
{
    // Capture-and-replay state for the editor's Frame Debugger panel. Implements RG::IArchiveSink so
    // the render graph copies tracked render-targets into owned VkImages post-pass. The panel can
    // then preview each pass's output and re-record individual draws via ImmediateSubmit while the
    // live graph stays paused in DebuggerState::Frozen. One instance lives on RenderingSystem.
    enum class DebuggerState : u8 { Inactive, CaptureRequested, Frozen };

    // Which view the user has chosen to capture next (and to overlay during Frozen).
    // The corresponding viewport's RG installs the archive sink.
    enum class CaptureSource : u8 { Scene, Game };

    struct FrameDebugger : public RG::IArchiveSink
    {
        // Capture state machine
        DebuggerState     state = DebuggerState::Inactive;
        RG::CapturedFrame capturedFrame;

        // User's pending source selection (drives the next capture).
        // capturedSource records what was actually captured (set when state
        // transitions to Frozen) so the viewport overlay survives the user
        // toggling requestedSource between captures.
        CaptureSource requestedSource = CaptureSource::Scene;
        CaptureSource capturedSource  = CaptureSource::Scene;

        // Frame index stamp for the Frozen-state cameraMoved throttle in
        // RenderingSystem (caps auto-refresh at ~10 Hz so the per-recapture
        // GPU copy bandwidth doesn't saturate mid-tier GPUs).
        u64 lastRecaptureFrameIndex = 0;

        // Per-draw stepping reads the frozen UBOs/SSBOs/indirect buffers and
        // re-records the owning pass via ImmediateSubmit; live re-replay is gone.

        // Debug blit resources (depth -> color preview composition)
        std::unique_ptr<VKPipeline>  blitPipeline;
        std::unique_ptr<VKPipeline>  depthPipeline;
        std::unique_ptr<VKPipeline>  slimDecodePipeline; // SlimNormal/SlimMotion/SlimRoughness (float-sampled)
        std::unique_ptr<VKPipeline>  slimMatIDPipeline;  // SlimMaterialID (usampler2D R16U)
        std::vector<u32>             blitFragSpv;
        std::vector<u32>             depthFragSpv;
        std::vector<u32>             slimDecodeFragSpv;
        std::vector<u32>             slimMatIDFragSpv;
        VkDescriptorPool             descPool      = VK_NULL_HANDLE;
        VkDescriptorSetLayout        descSetLayout = VK_NULL_HANDLE;
        VkDescriptorSet              descSet       = VK_NULL_HANDLE;
        VkSampler                    sampler       = VK_NULL_HANDLE;

        // Archive sink configuration. Names match RG::TextureDesc::name.
        // Set by RegisterTrackedRT before each capture.
        std::unordered_set<std::string> trackedRTs;

        // (passName + "/" + rtName) → index into capturedFrame.archivedImages.
        // Survives across captures so OnPassExecuted reuses the same VkImage
        // / VkImageView (key + dims/format match) instead of reallocating —
        // panel and viewport descriptor caches keyed by view-pointer stay
        // valid frame-to-frame. Cleared only by DestroyArchives.
        std::unordered_map<std::string, u32> m_ArchiveSlotMap;
        // Slots written this capture; FinalizeCapture frees stragglers (e.g.
        // an entry from a prior capture's SelectionMaskPass after the user
        // toggled drawSelectionOutline off). Reset at BeginCapture.
        std::unordered_set<std::string>      m_ArchiveSlotInUseThisCapture;

        // Cached device/allocator for archive ownership. Populated by BeginCapture.
        VkDevice     archiveDevice    = VK_NULL_HANDLE;
        VmaAllocator archiveAllocator = nullptr;

        // Capture helpers (called during normal recording when CaptureRequested).
        // graphPassIndex is RenderPassContext::passIndex — needed so the Frozen
        // panel can key passArchives lookups by graph index instead of dense
        // push order. Pass call sites grab it from ctx.passIndex.
        void BeginCapturePass(u32 graphPassIndex,
                              const std::string& name, const std::string& activeTarget,
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

        // Archive lifecycle (called from RenderingSystem around the capture frame).
        void BeginCapture(VkDevice device, VmaAllocator allocator);
        void RegisterTrackedRT(const std::string& name);
        void FinalizeCapture(const Mat4& viewProj);
        void DestroyArchives();

        // IArchiveSink — invoked post-pass during RenderGraph::Execute. queueFamily reflects which queue's primary
        // primaryCmd was recorded on; the sink substitutes compute-compatible stage masks when on the compute primary.
        void OnPassExecuted(u32 passIndex, RG::RenderGraph& graph, VkCommandBuffer primaryCmd,
                            RG::QueueFamily queueFamily) override;

        void Shutdown(VkDevice device);
    };
}
