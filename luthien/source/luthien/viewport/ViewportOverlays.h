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

    class ViewportOverlays
    {
    public:
        ViewportOverlays(ViewportRenderer& viewport, GizmoController& gizmo)
            : m_Viewport(viewport), m_Gizmo(gizmo) {}

        // Draws all four world-space overlays gated by EditorSettings flags.
        void DrawAll(const std::shared_ptr<Scene>& scene,
                     const EditorCamera& camera,
                     Entity selected);

    private:
        void DrawBoneDebug(const EditorCamera& camera, Entity selected);
        void DrawLights(const std::shared_ptr<Scene>& scene,
                        const EditorCamera& camera, Entity selected);
        void DrawCameras(const std::shared_ptr<Scene>& scene,
                         const EditorCamera& camera, Entity selected);
        void DrawAABBs(const std::shared_ptr<Scene>& scene,
                       const EditorCamera& camera);

        ImVec2 ProjectToScreen(const EditorCamera& camera, const Vec3& worldPos) const;
        bool   IsInViewport(const ImVec2& p) const;
        static ImU32 LightColorToImU32(const Vec3& color, float alpha = 0.85f);
        bool   ClipLineToNearPlane(const EditorCamera& camera, Vec3& a, Vec3& b) const;
        void   DrawClippedLine(ImDrawList* drawList, const EditorCamera& camera,
                               const Vec3& worldA, const Vec3& worldB,
                               ImU32 color, float thickness = 1.0f);

        ViewportRenderer& m_Viewport;
        GizmoController&  m_Gizmo;
    };
}
