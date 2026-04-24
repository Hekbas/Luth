#pragma once

#include "luth/core/types/LuthTypes.h"
#include "luth/core/types/LuthMath.h"

#include <imgui.h>
#include <vulkan/vulkan.h>
#include <functional>
#include <memory>

namespace Luth
{
    class RenderingSystem;
    class Texture;

    class ViewportRenderer
    {
    public:
        using ResizeFn = std::function<void(u32, u32)>;

        ViewportRenderer() = default;
        ~ViewportRenderer();

        // Detect size change (fires m_OnResize) + capture bounds/focus/hover.
        // Called once per frame just before DrawSceneTexture, inside the
        // owning ImGui window.
        //
        // aspectRatio = 0: free aspect — inner rect fills the panel content
        // region (Scene panel behaviour). aspectRatio > 0: lock the inner
        // rect to that aspect and center within the panel (Game panel —
        // mimics Unity's game view letterbox/pillarbox).
        void BeginViewport(float aspectRatio = 0.0f);

        // Rebind descriptor set if scene texture changed, then ImGui::Image it.
        // Scene panel overload reads RenderingSystem::GetSceneColor(); the direct
        // texture overload is used by GamePanel (its own FrameTargets LDR output).
        void DrawSceneTexture(RenderingSystem* renderingSystem);
        void DrawSceneTexture(const std::shared_ptr<Texture>& texture);

        // Applied from the panel's resize callback so viewport state stays in sync
        // with the authoritative size used by RenderingSystem + EditorCamera.
        void SetSize(u32 width, u32 height);

        // Per-instance resize handler fired by BeginViewport on dimension change.
        // Scene panel: resizes RS::m_SceneTargets + EditorCamera. Game panel:
        // resizes its own FrameTargets. Replaces the single-subscriber
        // RenderResizeEvent bus that assumed one scene viewport.
        void SetOnResize(ResizeFn fn) { m_OnResize = std::move(fn); }

        const Vec2&  GetSize()    const { return m_Size; }
        const ImVec2* GetBounds() const { return m_Bounds; }
        bool IsFocused() const { return m_IsFocused; }
        bool IsHovered() const { return m_IsHovered; }

    private:
        Vec2   m_Size = { 0.0f, 0.0f };
        ImVec2 m_Bounds[2] = {};
        bool   m_IsFocused = false;
        bool   m_IsHovered = false;

        ResizeFn m_OnResize;

        // Per-instance to avoid leaking across editor teardown.
        VkDescriptorSet          m_SceneDS = VK_NULL_HANDLE;
        std::shared_ptr<Texture> m_LastSceneTex = nullptr;
    };
}
