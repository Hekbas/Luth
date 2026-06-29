#include "lepch.h"
#include "luthien/viewport/ViewportOverlays.h"

#include "luthien/viewport/ViewportRenderer.h"
#include "luthien/viewport/GizmoController.h"
#include "luthien/EditorCamera.h"
#include "luthien/EditorSettings.h"
#include "luthien/EditorColors.h"
#include "luthien/Editor.h"
#include "luthien/widgets/Icons.h"
#include "luth/scene/Scene.h"
#include "luth/scene/Components.h"

namespace Luth
{
    using namespace Component;

    // The in-world wireframe gizmos (light ranges, camera frustums, AABBs, bone skeletons) render
    // through the engine's GPU debug-draw (luth/scene/systems/DebugGizmos). What remains here are the
    // 2D billboarded selection icons, which also act as click-to-select hit targets.

    void ViewportOverlays::DrawAll(const std::shared_ptr<Scene>& scene,
                                   const EditorCamera& camera, Entity selected)
    {
        DrawLights(scene, camera, selected);
        DrawCameras(scene, camera, selected);
    }

    ImVec2 ViewportOverlays::ProjectToScreen(const EditorCamera& camera, const Vec3& worldPos) const
    {
        Vec4 clipPos = camera.GetViewProjection() * Vec4(worldPos, 1.0f);
        if (clipPos.w <= 0.001f) return { -1.0f, -1.0f };
        Vec3 ndc = Vec3(clipPos) / clipPos.w;
        const ImVec2* bounds = m_Viewport.GetBounds();
        const Vec2& size = m_Viewport.GetSize();
        float screenX = bounds[0].x + (ndc.x * 0.5f + 0.5f) * size.x;
        float screenY = bounds[0].y + (-ndc.y * 0.5f + 0.5f) * size.y;
        return { screenX, screenY };
    }

    ImU32 ViewportOverlays::LightColorToImU32(const Vec3& color, float alpha)
    {
        return IM_COL32(
            (u8)(Math::Clamp(color.r, 0.0f, 1.0f) * 255.0f),
            (u8)(Math::Clamp(color.g, 0.0f, 1.0f) * 255.0f),
            (u8)(Math::Clamp(color.b, 0.0f, 1.0f) * 255.0f),
            (u8)(alpha * 255.0f));
    }

    // ── Light Icons ──

    void ViewportOverlays::DrawLights(const std::shared_ptr<Scene>& scene,
                                      const EditorCamera& camera, Entity selected)
    {
        const auto& gs = Editor::GetSettings();
        if (!gs.lightsSelected && !gs.lightsAll) return;   // icons show whenever lights are enabled (either scope)
        if (!scene) return;

        auto& registry = scene->Registry();
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        const ImVec2* bounds = m_Viewport.GetBounds();
        drawList->PushClipRect(bounds[0], bounds[1], true);

        const bool isHovered = m_Viewport.IsHovered();
        const bool hasValidSelection = selected && selected.IsValid();

        auto icon = [&](entt::entity e, const Vec3& worldPos, const char* glyph, ImU32 color) {
            ImVec2 screenPos = ProjectToScreen(camera, worldPos);
            if (screenPos.x >= 0.0f)
                m_Gizmo.DrawGizmoIcon(drawList, screenPos, glyph, color, e, isHovered, hasValidSelection);
        };

        for (auto e : registry.view<WorldTransform, DirectionalLight>()) {
            auto& wt = registry.get<WorldTransform>(e);
            auto& dl = registry.get<DirectionalLight>(e);
            icon(e, Vec3(wt.Matrix[3]), ICON_LIGHT_DIRECTIONAL_FILL, LightColorToImU32(dl.Color));
        }
        for (auto e : registry.view<WorldTransform, PointLight>()) {
            auto& wt = registry.get<WorldTransform>(e);
            auto& pl = registry.get<PointLight>(e);
            icon(e, Vec3(wt.Matrix[3]), ICON_LIGHT_POINT_FILL, LightColorToImU32(pl.Color));
        }
        for (auto e : registry.view<WorldTransform, SpotLight>()) {
            auto& wt = registry.get<WorldTransform>(e);
            auto& sl = registry.get<SpotLight>(e);
            icon(e, Vec3(wt.Matrix[3]), ICON_LIGHT_SPOT_FILL, LightColorToImU32(sl.Color));
        }

        drawList->PopClipRect();
    }

    // ── Camera Icons ──

    void ViewportOverlays::DrawCameras(const std::shared_ptr<Scene>& scene,
                                       const EditorCamera& camera, Entity selected)
    {
        const auto& gs = Editor::GetSettings();
        if (!gs.camerasSelected && !gs.camerasAll) return;   // icons show whenever cameras are enabled (either scope)
        if (!scene) return;

        auto& registry = scene->Registry();
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        const ImVec2* bounds = m_Viewport.GetBounds();
        drawList->PushClipRect(bounds[0], bounds[1], true);

        const bool isHovered = m_Viewport.IsHovered();
        const bool hasValidSelection = selected && selected.IsValid();

        for (auto e : registry.view<WorldTransform, Camera>()) {
            auto& wt = registry.get<WorldTransform>(e);
            ImVec2 screenPos = ProjectToScreen(camera, Vec3(wt.Matrix[3]));
            if (screenPos.x >= 0.0f)
                m_Gizmo.DrawGizmoIcon(drawList, screenPos, ICON_VIDEO_CAMERA_FILL,
                                      EditorColors::GizmoCamera, e, isHovered, hasValidSelection);
        }

        drawList->PopClipRect();
    }
}
