#pragma once

#include "luthien/Editor.h"
#include "luthien/EditorCamera.h"
#include "luthien/viewport/ViewportRenderer.h"
#include "luthien/viewport/GizmoController.h"
#include "luth/scene/Entity.h"
#include "luth/scene/systems/RenderingSystem.h"
#include "luth/events/Event.h"
#include "luth/events/EventBus.h"

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
        void DrawBoneDebugOverlay();
        void DrawLightGizmos();
        void DrawCameraGizmos();
        void DrawAABBGizmos();

        ImVec2 ProjectToScreen(const Vec3& worldPos) const;
        bool   IsInViewport(const ImVec2& p) const;
        ImU32  LightColorToImU32(const Vec3& color, float alpha = 0.85f) const;
        bool   ClipLineToNearPlane(Vec3& a, Vec3& b) const;
        void   DrawClippedLine(ImDrawList* drawList, const Vec3& worldA, const Vec3& worldB,
                               ImU32 color, float thickness = 1.0f);

        std::shared_ptr<Scene> m_Context;
        RenderingSystem* m_RenderingSystem = nullptr;
        EditorCamera m_EditorCamera;
        std::unique_ptr<ViewportRenderer> m_Viewport;
        std::unique_ptr<GizmoController>  m_Gizmo;

        Entity m_SelectedEntity;
        bool m_ShowControlsOverlay = true;
    };
}
