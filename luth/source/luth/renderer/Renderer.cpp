#include "luthpch.h"
#include "luth/renderer/Renderer.h"
#include "luth/renderer/material/MaterialSystem.h"
#include "luth/jobs/JobSystem.h"
#include "luth/renderer/rendergraph/RenderGraph.h"
#include "luth/renderer/backend/vulkan/CommandAllocatorPool.h"
#include "luth/renderer/backend/vulkan/VulkanBackend.h"
#include "luth/renderer/backend/vulkan/VulkanContext.h"
#include "luth/renderer/backend/vulkan/GpuTracy.h"

namespace Luth
{
    std::unique_ptr<RenderBackend> Renderer::s_Backend = nullptr;
    FrameData* Renderer::s_FrameData = nullptr;

    void Renderer::Init(void* windowHandle)
    {
        s_Backend = RenderBackend::Create();
        s_Backend->Init(windowHandle);
        MaterialSystem::Init();
    }

    void Renderer::Shutdown()
    {
        // Wait for all GPU work to complete BEFORE destroying any resources. MaterialSystem::Shutdown() frees
        // its SSBO buffer directly (no deferred deletion), so in-flight command buffers must have finished first.
        WaitForGPU();
        MaterialSystem::Shutdown();
        if (s_Backend) {
            s_Backend->Shutdown();
            s_Backend.reset();
        }
    }

    void Renderer::WaitForGPU()
    {
        vkDeviceWaitIdle(VulkanContext::Get().GetDevice());
    }

    void Renderer::FlushDeletionQueues()
    {
        VulkanContext::Get().FlushAllDeletionQueues();
    }

    void Renderer::SetFrameData(FrameData* frameData)
    {
        s_FrameData = frameData;
    }

    bool Renderer::BeginFrame(u64 frameIndex)
    {
        return s_Backend->AcquireImage(frameIndex);
    }

    void Renderer::EndFrame()
    {
        // No-op; submission happens in ExecuteGraph
    }

    void Renderer::OnResize(u32 width, u32 height)
    {
        s_Backend->OnResize(width, height);
    }

    QueueRecorders Renderer::BeginPrimaryCmd(u64 frameIndex, u32 viewSlot)
    {
        auto* vk = static_cast<VulkanBackend*>(s_Backend.get());
        QueueRecorders recorders {
            vk->GetGraphicsAPrimary(frameIndex, viewSlot),
            vk->GetComputePrimary  (frameIndex, viewSlot),
            vk->GetGraphicsBPrimary(frameIndex, viewSlot),
        };
        VkCommandBufferBeginInfo beginInfo{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(recorders.gA,      &beginInfo);
        vkBeginCommandBuffer(recorders.compute, &beginInfo);
        vkBeginCommandBuffer(recorders.gB,      &beginInfo);
        return recorders;
    }

    bool Renderer::RecordGraph(QueueRecorders recorders, RG::RenderGraph& graph, GPUTimerPool* timers)
    {
        // RG::Execute routes per pass: AsyncCompute -> recorders.compute; Graphics -> recorders.gA (before first
        // AsyncCompute pass) or recorders.gB (after). Returns true iff any pass routed to compute, so SubmitView
        // can skip the compute submit when the graph stayed graphics-only.
        return graph.Execute(recorders, timers);
    }

    void Renderer::EndPrimaryCmdAndSubmit(QueueRecorders recorders, u64 frameIndex, u32 viewSlot,
                                          bool hasComputeWork, bool isLastView)
    {
    #if defined(TRACY_ENABLE)
        // Tracy GPU collect: append the readback to the still-open primaries. Graphics ctx on gB (always
        // submitted, last graphics submit); compute ctx on the compute primary only when it carries work and is
        // a distinct async context (else it aliases graphics and is collected via gB).
        auto& vkCtx = VulkanContext::Get();
        LH_PROFILE_GPU_COLLECT(vkCtx.GetGraphicsTracyCtx(), recorders.gB);
        if (hasComputeWork && vkCtx.GetComputeTracyCtx() != vkCtx.GetGraphicsTracyCtx())
            LH_PROFILE_GPU_COLLECT(vkCtx.GetComputeTracyCtx(), recorders.compute);
    #endif

        // Present transition is RG-driven; ImGuiPass imports the backbuffer with finalState=Present. End all
        // three primaries (empty compute/gB are valid no-op submits) and forward to the backend's per-view 3-submit
        // topology. SubmitView skips the compute submit when hasComputeWork is false; gB always submits.
        vkEndCommandBuffer(recorders.gA);
        vkEndCommandBuffer(recorders.compute);
        vkEndCommandBuffer(recorders.gB);
        s_Backend->SubmitView(frameIndex, viewSlot, recorders, hasComputeWork, isLastView);
    }

    void Renderer::ExecuteGraph(RG::RenderGraph& graph, u64 frameIndex, GPUTimerPool* timers)
    {
        QueueRecorders recorders = BeginPrimaryCmd(frameIndex, /*viewSlot=*/0);
        const bool hasComputeWork = RecordGraph(recorders, graph, timers);
        EndPrimaryCmdAndSubmit(recorders, frameIndex, /*viewSlot=*/0, hasComputeWork, /*isLastView=*/true);
    }
}
