#include "lepch.h"
#include "luthien/viewport/ViewportRenderer.h"

#include "luthien/widgets/ImGuiUtils.h"
#include "luth/events/RenderEvent.h"
#include "luth/events/EventBus.h"
#include "luth/scene/systems/RenderingSystem.h"
#include "luth/renderer/backend/vulkan/VulkanContext.h"
#include "luth/renderer/backend/vulkan/VulkanTexture.h"

#include <backends/imgui_impl_vulkan.h>

namespace Luth
{
    ViewportRenderer::~ViewportRenderer()
    {
        if (m_SceneDS) {
            ImGui_ImplVulkan_RemoveTexture(m_SceneDS);
            m_SceneDS = VK_NULL_HANDLE;
        }
        m_LastSceneTex.reset();
    }

    void ViewportRenderer::BeginViewport()
    {
        // Viewport sizing — compare as integers to avoid an infinite resize
        // loop caused by float → u32 truncation in the RenderResizeEvent path.
        const Vec2 avail = ToGlmVec2(ImGui::GetContentRegionAvail());
        const u32 newW = (u32)avail.x;
        const u32 newH = (u32)avail.y;
        const u32 curW = (u32)m_Size.x;
        const u32 curH = (u32)m_Size.y;
        if ((newW != curW || newH != curH) && newW > 0 && newH > 0) {
            m_Size = { (float)newW, (float)newH };
            EventBus::Enqueue<RenderResizeEvent>(BusType::MainThread, newW, newH);
        }

        // Update viewport bounds for gizmos & mouse picking
        ImVec2 cursorScreenPos = ImGui::GetCursorScreenPos();
        m_Bounds[0] = cursorScreenPos;
        m_Bounds[1] = { cursorScreenPos.x + m_Size.x, cursorScreenPos.y + m_Size.y };
    }

    void ViewportRenderer::DrawSceneTexture(RenderingSystem* renderingSystem)
    {
        if (auto texture = renderingSystem->GetSceneColor())
        {
            if (texture != m_LastSceneTex)
            {
                if (m_SceneDS)
                {
                    VkDescriptorSet oldSet = m_SceneDS;
                    VulkanContext::Get().PushDeletion([oldSet]() {
                        ImGui_ImplVulkan_RemoveTexture(oldSet);
                    });
                }

                auto vkTex = std::static_pointer_cast<VKTexture>(texture);
                m_SceneDS = ImGui_ImplVulkan_AddTexture(vkTex->GetSampler(), vkTex->GetImageView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                m_LastSceneTex = texture;
            }

            ImGui::Image((ImTextureID)m_SceneDS, ToImVec2(m_Size), { 0, 0 }, { 1, 1 });
        }
        else
        {
            ImGui::Text("No Scene Output");
        }

        // Interaction states (sampled after Image to make IsWindowHovered reflect the viewport area)
        m_IsFocused = ImGui::IsWindowFocused();
        m_IsHovered = ImGui::IsWindowHovered();
    }

    void ViewportRenderer::SetSize(u32 width, u32 height)
    {
        m_Size = { (float)width, (float)height };
    }
}
