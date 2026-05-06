#include "luthpch.h"
#include "luth/scene/systems/RenderingSystem.h"
#include "luth/renderer/RenderPipeline.h"
#include "luth/core/diagnostics/Profiler.h"
#include "luth/scene/Scene.h"
#include "luth/scene/Components.h"
#include "luth/renderer/Renderer.h"
#include "luth/renderer/material/MaterialSystem.h"
#include "luth/renderer/resources/BoneMatrixBuffer.h"
#include "luth/renderer/backend/vulkan/VulkanBackend.h"
#include "luth/renderer/backend/vulkan/VulkanContext.h"
#include "luth/renderer/backend/vulkan/VulkanTexture.h"
#include "luth/renderer/backend/vulkan/VulkanBuffer.h"
#include "luth/renderer/material/Material.h"
#include "luth/renderer/resources/Model.h"
#include "luth/resources/AssetManager.h"
#include <vma/vk_mem_alloc.h>
#include <backends/imgui_impl_vulkan.h>
#include <imgui.h>

namespace Luth
{
    using namespace Component;

    void RenderPipeline::AddImGuiPass(RG::RenderGraph& rg, RG::ResourceHandle sceneColor)
    {
        struct ImGuiPassData {
            RG::ResourceHandle backbuffer;
            RG::ResourceHandle sceneTexture;
        };

        rg.AddPass<ImGuiPassData>("ImGuiPass",
            [&](ImGuiPassData& data, RG::RenderPassBuilder& builder)
            {
                auto* vkRenderer = static_cast<VulkanBackend*>(Renderer::GetBackend());
                VkImage     swapchainImage = vkRenderer->GetSwapchain().GetImage(vkRenderer->GetSwapchain().GetCurrentFrameIndex());
                VkImageView swapchainView  = vkRenderer->GetSwapchain().GetImageView(vkRenderer->GetSwapchain().GetCurrentFrameIndex());

                RG::TextureDesc desc;
                desc.name   = "Backbuffer";
                desc.width  = vkRenderer->GetSwapchain().GetExtent().width;
                desc.height = vkRenderer->GetSwapchain().GetExtent().height;
                desc.format = RG::TextureFormat::BGRA8_Unorm;

                // finalState=Present → RG appends the present-barrier on this pass.
                data.backbuffer = rg.ImportResource(desc,
                    (void*)swapchainImage, (void*)swapchainView,
                    RG::ResourceState::Undefined, RG::ResourceState::Present);

                data.backbuffer = builder.Write(data.backbuffer);

                if (sceneColor.IsValid())
                    data.sceneTexture = builder.Read(sceneColor);
            },
            [this](ImGuiPassData& data, RG::RenderPassContext& ctx)
            {
                m_System.GetFrameDebugger().BeginCapturePass(ctx.passIndex, "ImGuiPass", "Backbuffer", false,
                    { "imgui", 0, VK_CULL_MODE_NONE, VK_POLYGON_MODE_FILL, false, false, false, true });
                ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), ctx.commandBuffer);
                m_System.GetFrameDebugger().EndCapturePass();
            }
        );
    }

}
