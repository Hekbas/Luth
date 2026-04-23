#pragma once

#include "luth/core/types/LuthTypes.h"
#include "luth/core/types/LuthMath.h"

#include <imgui.h>
#include <vulkan/vulkan.h>
#include <memory>

namespace Luth
{
    class RenderingSystem;
    class Texture;

    class ViewportRenderer
    {
    public:
        ViewportRenderer() = default;
        ~ViewportRenderer();

        // Detect size change (enqueues RenderResizeEvent) + capture bounds/focus/hover.
        // Called once per frame just before DrawSceneTexture, inside the owning ImGui window.
        void BeginViewport();

        // Rebind descriptor set if scene texture changed, then ImGui::Image it.
        void DrawSceneTexture(RenderingSystem* renderingSystem);

        // Applied from the panel's RenderResizeEvent handler so viewport state stays in sync
        // with the authoritative size used by RenderingSystem + EditorCamera.
        void SetSize(u32 width, u32 height);

        const Vec2&  GetSize()    const { return m_Size; }
        const ImVec2* GetBounds() const { return m_Bounds; }
        bool IsFocused() const { return m_IsFocused; }
        bool IsHovered() const { return m_IsHovered; }

    private:
        Vec2   m_Size = { 0.0f, 0.0f };
        ImVec2 m_Bounds[2] = {};
        bool   m_IsFocused = false;
        bool   m_IsHovered = false;

        // Per-instance to avoid leaking across editor teardown.
        VkDescriptorSet          m_SceneDS = VK_NULL_HANDLE;
        std::shared_ptr<Texture> m_LastSceneTex = nullptr;
    };
}
