#pragma once

#include "luthien/Editor.h"
#include "luthien/EditorCamera.h"
#include "luthien/viewport/ViewportRenderer.h"
#include "luthien/viewport/GizmoController.h"
#include "luthien/viewport/ViewportOverlays.h"
#include "luth/scene/Entity.h"
#include "luth/scene/systems/RenderingSystem.h"

#include <memory>

namespace Luth
{
    class Scene;

    // Scene panel is dominantly ImGui-driven (gizmo input, viewport image, drag-drop),
    // so the snapshot is intentionally tiny — most work belongs in OnDraw. Future polish
    // can capture EditorCamera matrices here so the EditorViewportState handoff happens
    // in gather instead.
    struct SceneViewportSnapshot
    {
        u32 selectionVersion = 0;
    };

    class ScenePanel : public Panel
    {
    public:
        ScenePanel(RenderingSystem* renderingSystem);
        ~ScenePanel() override;

        void OnInit() override;
        void OnGather(EditorSnapshotBuilder& builder) override;
        void OnDraw(const EditorSnapshot& snapshot) override;

        void SetContext(const std::shared_ptr<Scene>& context) { m_Context = context; }

        bool IsViewportFocused() const { return m_Viewport->IsFocused(); }
        bool IsViewportHovered() const { return m_Viewport->IsHovered(); }

        EditorCamera& GetEditorCamera() { return m_EditorCamera; }

        bool GetShowControlsOverlay() const { return m_ShowControlsOverlay; }
        void SetShowControlsOverlay(bool show) { m_ShowControlsOverlay = show; }

    private:
        std::shared_ptr<Scene> m_Context;
        RenderingSystem* m_RenderingSystem = nullptr;
        EditorCamera m_EditorCamera;
        std::unique_ptr<ViewportRenderer> m_Viewport;
        std::unique_ptr<GizmoController>  m_Gizmo;
        std::unique_ptr<ViewportOverlays> m_Overlays;

        Entity m_SelectedEntity;
        bool m_ShowControlsOverlay = true;
    };
}
