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

    // ImGui::Image wrapper for displaying RenderingSystem's scene color target inside an editor panel.
    // Owns the ImGui descriptor set used for the bind, detects panel resize (which drives
    // RenderingSystem::Resize), and captures bounds / focus / hover state so ScenePanel and GamePanel
    // can correctly gate input handling.
    class ViewportRenderer
    {
    public:
        using ResizeFn = std::function<void(u32, u32)>;

        ViewportRenderer() = default;
        ~ViewportRenderer();

        // Detect size change (fires m_OnResize) + capture bounds/focus/hover. Call once per frame just
        // before DrawSceneTexture. aspectRatio == 0: inner rect fills the panel (Scene panel).
        // aspectRatio > 0: lock to that aspect and center, letterbox/pillarbox bars filling the
        // remainder (Game panel).
        void BeginViewport(float aspectRatio = 0.0f);

        // Rebind descriptor set if scene texture changed, then ImGui::Image it. Scene panel overload
        // reads RenderingSystem::GetSceneColor(); the direct texture overload is used by GamePanel (its
        // own FrameTargets LDR output).
        void DrawSceneTexture(RenderingSystem* renderingSystem);
        void DrawSceneTexture(const std::shared_ptr<Texture>& texture);

        // Raw-handle overload used by the FrameDebugger viewport overlay to bind an archived RT
        // (VkImageView owned by FrameDebugger, not a Texture wrapper) directly into ImGui. Caches the
        // descriptor by view-pointer identity; recapture invalidates because new views replace old ones.
        void DrawSceneTextureRaw(VkImageView view, VkSampler sampler);

        // Applied from the panel's resize callback so viewport state stays in sync with the
        // authoritative size used by RenderingSystem + EditorCamera.
        void SetSize(u32 width, u32 height);

        // Fired by BeginViewport on dimension change. Scene panel resizes RS::m_SceneTargets +
        // EditorCamera; GamePanel resizes its own FrameTargets. Per-instance so multi-view stays
        // independent.
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

        // Separate cache for the raw-view overlay path so toggling the overlay on/off doesn't churn
        // the live-scene descriptor.
        VkDescriptorSet          m_RawDS         = VK_NULL_HANDLE;
        VkImageView              m_RawViewCached = VK_NULL_HANDLE;
    };
}
