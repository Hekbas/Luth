#include "luthpch.h"
#include "luth/renderer/Renderer.h"
#include "luth/renderer/material/MaterialSystem.h"
#include "luth/jobs/JobSystem.h"
#include "luth/renderer/rendergraph/RenderGraph.h"
#include "luth/renderer/backend/vulkan/CommandAllocatorPool.h"
#include "luth/renderer/backend/vulkan/VulkanBackend.h"
#include "luth/renderer/backend/vulkan/VulkanContext.h"

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
        // Wait for all GPU work to complete BEFORE destroying any resources.
        // MaterialSystem::Shutdown() frees its SSBO buffer directly (no deferred
        // deletion), so in-flight command buffers must have finished first.
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
        // No-op — submission happens in ExecuteGraph
    }

    void Renderer::OnResize(u32 width, u32 height)
    {
        s_Backend->OnResize(width, height);
    }

    void* Renderer::BeginPrimaryCmd(u64 frameIndex)
    {
        VkCommandBuffer primaryCmd = (VkCommandBuffer)s_Backend->GetFrameCommandBuffer(frameIndex);
        VkCommandBufferBeginInfo beginInfo{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(primaryCmd, &beginInfo);
        return primaryCmd;
    }

    void Renderer::RecordGraph(void* cmd, RG::RenderGraph& graph, GPUTimerPool* timers)
    {
        graph.Execute((VkCommandBuffer)cmd, timers);
    }

    void Renderer::EndPrimaryCmdAndSubmit(void* cmd, u64 frameIndex)
    {
        // Present transition is RG-driven — ImGuiPass imports the backbuffer with finalState=Present.
        VkCommandBuffer primaryCmd = (VkCommandBuffer)cmd;
        vkEndCommandBuffer(primaryCmd);
        s_Backend->SubmitFrame(frameIndex, primaryCmd);
    }

    void Renderer::ExecuteGraph(RG::RenderGraph& graph, u64 frameIndex, GPUTimerPool* timers)
    {
        void* cmd = BeginPrimaryCmd(frameIndex);
        RecordGraph(cmd, graph, timers);
        EndPrimaryCmdAndSubmit(cmd, frameIndex);
    }
}
