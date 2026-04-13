#include "luthpch.h"
#include "luth/scene/systems/RenderingSystem.h"
#include "luth/core/Profiler.h"
#include "luth/scene/Scene.h"
#include "luth/scene/Components.h"
#include "luth/renderer/Renderer.h"
#include "luth/renderer/MaterialSystem.h"
#include "luth/renderer/BoneMatrixBuffer.h"
#include "luth/renderer/backend/vulkan/VulkanBackend.h"
#include "luth/renderer/backend/vulkan/VulkanContext.h"
#include "luth/renderer/backend/vulkan/VulkanTexture.h"
#include "luth/renderer/backend/vulkan/VulkanBuffer.h"
#include "luth/renderer/Material.h"
#include "luth/renderer/Model.h"
#include "luth/resources/AssetManager.h"
#include "luth/renderer/ShaderCompiler.h"
#include <glm/gtc/matrix_transform.hpp>
#include <vma/vk_mem_alloc.h>
#include <backends/imgui_impl_vulkan.h>
#include <imgui.h>

namespace Luth
{
    using namespace Component;

    void RenderingSystem::AddImGuiPass(RG::RenderGraph& rg, RG::ResourceHandle sceneColor)
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

                data.backbuffer = rg.ImportResource(desc,
                    (void*)swapchainImage, (void*)swapchainView,
                    RG::ResourceState::Undefined);

                data.backbuffer = builder.Write(data.backbuffer);

                if (sceneColor.IsValid())
                    data.sceneTexture = builder.Read(sceneColor);
            },
            [this](ImGuiPassData& data, RG::RenderPassContext& ctx)
            {
                m_FrameDebugger.BeginCapturePass("ImGuiPass", "Backbuffer", false,
                    { "imgui", 0, VK_CULL_MODE_NONE, VK_POLYGON_MODE_FILL, false, false, false, true });
                ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), ctx.commandBuffer);
                m_FrameDebugger.EndCapturePass();
            }
        );
    }

}
