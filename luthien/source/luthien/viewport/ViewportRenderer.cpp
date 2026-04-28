#include "lepch.h"
#include "luthien/viewport/ViewportRenderer.h"

#include "luthien/widgets/ImGuiUtils.h"
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
        if (m_RawDS) {
            ImGui_ImplVulkan_RemoveTexture(m_RawDS);
            m_RawDS = VK_NULL_HANDLE;
        }
        m_LastSceneTex.reset();
    }

    void ViewportRenderer::BeginViewport(float aspectRatio)
    {
        const Vec2 avail = ToGlmVec2(ImGui::GetContentRegionAvail());

        Vec2 innerSize = avail;
        Vec2 offset    = { 0.0f, 0.0f };

        if (aspectRatio > 0.0f && avail.x > 0.0f && avail.y > 0.0f)
        {
            const float panelAspect = avail.x / avail.y;
            if (panelAspect > aspectRatio) {
                innerSize.x = avail.y * aspectRatio;
                innerSize.y = avail.y;
                offset.x    = (avail.x - innerSize.x) * 0.5f;  // pillarbox
            } else {
                innerSize.x = avail.x;
                innerSize.y = avail.x / aspectRatio;
                offset.y    = (avail.y - innerSize.y) * 0.5f;  // letterbox
            }
        }

        // Compare as integers — float → u32 truncation in the callback can
        // otherwise trigger an infinite resize loop.
        const u32 newW = (u32)innerSize.x;
        const u32 newH = (u32)innerSize.y;
        const u32 curW = (u32)m_Size.x;
        const u32 curH = (u32)m_Size.y;
        if ((newW != curW || newH != curH) && newW > 0 && newH > 0) {
            m_Size = { (float)newW, (float)newH };
            if (m_OnResize) m_OnResize(newW, newH);
        }

        if (offset.x > 0.0f || offset.y > 0.0f) {
            ImVec2 cursorPos = ImGui::GetCursorPos();
            ImGui::SetCursorPos({ cursorPos.x + offset.x, cursorPos.y + offset.y });
        }

        // Update viewport bounds for gizmos & mouse picking
        ImVec2 cursorScreenPos = ImGui::GetCursorScreenPos();
        m_Bounds[0] = cursorScreenPos;
        m_Bounds[1] = { cursorScreenPos.x + m_Size.x, cursorScreenPos.y + m_Size.y };
    }

    void ViewportRenderer::DrawSceneTexture(RenderingSystem* renderingSystem)
    {
        DrawSceneTexture(renderingSystem ? renderingSystem->GetSceneColor() : nullptr);
    }

    void ViewportRenderer::DrawSceneTexture(const std::shared_ptr<Texture>& texture)
    {
        if (texture)
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

    void ViewportRenderer::DrawSceneTextureRaw(VkImageView view, VkSampler sampler)
    {
        if (view == VK_NULL_HANDLE || sampler == VK_NULL_HANDLE)
        {
            ImGui::Text("No Scene Output");
            m_IsFocused = ImGui::IsWindowFocused();
            m_IsHovered = ImGui::IsWindowHovered();
            return;
        }

        if (view != m_RawViewCached)
        {
            if (m_RawDS)
            {
                VkDescriptorSet oldSet = m_RawDS;
                VulkanContext::Get().PushDeletion([oldSet]() {
                    ImGui_ImplVulkan_RemoveTexture(oldSet);
                });
            }
            m_RawDS = ImGui_ImplVulkan_AddTexture(sampler, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            m_RawViewCached = view;
        }

        if (m_RawDS != VK_NULL_HANDLE)
            ImGui::Image((ImTextureID)m_RawDS, ToImVec2(m_Size), { 0, 0 }, { 1, 1 });

        m_IsFocused = ImGui::IsWindowFocused();
        m_IsHovered = ImGui::IsWindowHovered();
    }

    void ViewportRenderer::SetSize(u32 width, u32 height)
    {
        m_Size = { (float)width, (float)height };
    }
}
