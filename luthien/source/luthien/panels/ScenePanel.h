#pragma once

#include "luthien/Editor.h"
#include "luthien/EditorCamera.h"
#include "luthien/viewport/ViewportRenderer.h"
#include "luth/scene/Entity.h"
#include "luth/scene/systems/RenderingSystem.h"
#include "luth/events/Event.h"
#include "luth/events/EventBus.h"

#include <ImGuizmo.h>
#include <memory>

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

        bool IsViewportFocused() const { return m_Viewport->IsFocused(); }
        bool IsViewportHovered() const { return m_Viewport->IsHovered(); }

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
        std::unique_ptr<ViewportRenderer> m_Viewport;

        Entity m_SelectedEntity;
        int m_GizmoType = -1; // -1 = none, otherwise ImGuizmo::OPERATION
        bool m_ShowTransformGizmo = true;

        // Captured on drag start so the whole drag coalesces into one undo entry.
        bool m_WasUsingGizmo = false;
        Vec3 m_GizmoStartPos{};
        Vec3 m_GizmoStartRot{};
        Vec3 m_GizmoStartScale{};

        // Icon click wins over pick result when both fire in the same frame.
        bool m_GizmoIconClicked = false;
        entt::entity m_GizmoIconEntity = entt::null;

        bool m_ShowControlsOverlay = true;
    };
}
