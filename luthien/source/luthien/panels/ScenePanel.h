#pragma once

#include "luthien/Editor.h"
#include "luthien/EditorCamera.h"
#include "luth/scene/Entity.h"
#include "luth/scene/systems/RenderingSystem.h"
#include "luth/events/Event.h"
#include "luth/events/EventBus.h"

#include <vulkan/vulkan.h>
#include <ImGuizmo.h>

namespace Luth
{
    class Scene;

    class ScenePanel : public Panel
    {
    public:
        ScenePanel(RenderingSystem* renderingSystem);
        ~ScenePanel() override;

        void OnInit() override;
        void OnRender() override;

        void SetContext(const std::shared_ptr<Scene>& context) { m_Context = context; }

        bool IsViewportFocused() const { return m_IsFocused; }
        bool IsViewportHovered() const { return m_IsHovered; }

        EditorCamera& GetEditorCamera() { return m_EditorCamera; }

        bool GetShowControlsOverlay() const { return m_ShowControlsOverlay; }
        void SetShowControlsOverlay(bool show) { m_ShowControlsOverlay = show; }

    private:
        void HandleRenderResize(Event& e);
        void DrawGizmos();
        void DrawBoneDebugOverlay();
        void DrawLightGizmos();
        void DrawCameraGizmos();
        void DrawAABBGizmos();

        // Shared gizmo helpers
        ImVec2 ProjectToScreen(const Vec3& worldPos) const;
        bool   IsInViewport(const ImVec2& p) const;
        ImU32  LightColorToImU32(const Vec3& color, float alpha = 0.85f) const;
        void   DrawGizmoIcon(ImDrawList* drawList, ImVec2 screenPos, const char* icon,
                             ImU32 color, entt::entity entity);
        bool   ClipLineToNearPlane(Vec3& a, Vec3& b) const;
        void   DrawClippedLine(ImDrawList* drawList, const Vec3& worldA, const Vec3& worldB,
                               ImU32 color, float thickness = 1.0f);

        std::shared_ptr<Scene> m_Context;
        RenderingSystem* m_RenderingSystem = nullptr;
        EditorCamera m_EditorCamera;

        Vec2 m_ViewportSize = { 0.0f, 0.0f };
        ImVec2 m_ViewportBounds[2];
        bool m_IsFocused = false;
        bool m_IsHovered = false;

        // Scene viewport texture tracking (must not be static — leaks on shutdown)
        VkDescriptorSet m_SceneDS = VK_NULL_HANDLE;
        std::shared_ptr<Texture> m_LastSceneTex = nullptr;

        // Gizmo state
        Entity m_SelectedEntity;
        int m_GizmoType = -1; // -1 = None, or ImGuizmo::OPERATION
        bool m_ShowTransformGizmo = true;

        // Gizmo drag tracking (for undo coalescing)
        bool m_WasUsingGizmo = false;
        Vec3 m_GizmoStartPos{};
        Vec3 m_GizmoStartRot{};
        Vec3 m_GizmoStartScale{};

        // Gizmo icon click tracking — deferred selection that wins over pick results
        bool m_GizmoIconClicked = false;
        entt::entity m_GizmoIconEntity = entt::null;

        // Controls overlay
        bool m_ShowControlsOverlay = true;
    };
}
