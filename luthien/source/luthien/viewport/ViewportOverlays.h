#pragma once

#include "luth/core/types/LuthMath.h"
#include "luth/scene/Entity.h"

#include <imgui.h>
#include <memory>

namespace Luth
{
    class Scene;
    class EditorCamera;
    class ViewportRenderer;
    class GizmoController;

    // 2D billboarded selection icons for lights, cameras, fog volumes, and wind, drawn over the Scene
    // viewport and gated by EditorSettings scopes. The icons double as click-to-select hit targets. The
    // in-world wireframe gizmos (ranges, frustums, AABBs, skeletons, fog shells, wind arrows) render via
    // the engine's GPU debug-draw (luth/scene/systems/DebugGizmos), not here.
    class ViewportOverlays
    {
    public:
        ViewportOverlays(ViewportRenderer& viewport, GizmoController& gizmo)
            : m_Viewport(viewport), m_Gizmo(gizmo) {}

        // Draws all selection-icon overlays gated by EditorSettings scopes.
        void DrawAll(const std::shared_ptr<Scene>& scene,
                     const EditorCamera& camera,
                     Entity selected);

    private:
        void DrawLights(const std::shared_ptr<Scene>& scene,
                        const EditorCamera& camera, Entity selected);
        void DrawCameras(const std::shared_ptr<Scene>& scene,
                         const EditorCamera& camera, Entity selected);
        void DrawFog(const std::shared_ptr<Scene>& scene,
                     const EditorCamera& camera, Entity selected);
        void DrawWind(const std::shared_ptr<Scene>& scene,
                      const EditorCamera& camera, Entity selected);

        ImVec2 ProjectToScreen(const EditorCamera& camera, const Vec3& worldPos) const;
        static ImU32 LightColorToImU32(const Vec3& color, float alpha = 0.85f);

        ViewportRenderer& m_Viewport;
        GizmoController&  m_Gizmo;
    };
}
