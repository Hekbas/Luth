#include "lepch.h"
#include "luthien/viewport/ViewportOverlays.h"

#include "luthien/viewport/ViewportRenderer.h"
#include "luthien/viewport/GizmoController.h"
#include "luthien/EditorCamera.h"
#include "luthien/EditorSelection.h"
#include "luthien/EditorSettings.h"
#include "luthien/EditorColors.h"
#include "luthien/Editor.h"
#include "luthien/widgets/Icons.h"
#include "luth/scene/Scene.h"
#include "luth/scene/Components.h"
#include "luth/renderer/resources/Model.h"
#include "luth/resources/AssetManager.h"

namespace Luth
{
    using namespace Component;

    void ViewportOverlays::DrawAll(const std::shared_ptr<Scene>& scene,
                                   const EditorCamera& camera, Entity selected)
    {
        DrawBoneDebug(camera, selected);
        DrawLights(scene, camera, selected);
        DrawCameras(scene, camera, selected);
        DrawAABBs(scene, camera);
    }

    // ── Shared helpers ──

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

    bool ViewportOverlays::IsInViewport(const ImVec2& p) const
    {
        const ImVec2* b = m_Viewport.GetBounds();
        return p.x >= b[0].x && p.x <= b[1].x
            && p.y >= b[0].y && p.y <= b[1].y;
    }

    ImU32 ViewportOverlays::LightColorToImU32(const Vec3& color, float alpha)
    {
        return IM_COL32(
            (u8)(Math::Clamp(color.r, 0.0f, 1.0f) * 255.0f),
            (u8)(Math::Clamp(color.g, 0.0f, 1.0f) * 255.0f),
            (u8)(Math::Clamp(color.b, 0.0f, 1.0f) * 255.0f),
            (u8)(alpha * 255.0f));
    }

    bool ViewportOverlays::ClipLineToNearPlane(const EditorCamera& camera, Vec3& a, Vec3& b) const
    {
        Mat4 vp = camera.GetViewProjection();
        // Compute clip-space w for each endpoint (w = row3 dot (x,y,z,1))
        float wa = vp[0][3] * a.x + vp[1][3] * a.y + vp[2][3] * a.z + vp[3][3];
        float wb = vp[0][3] * b.x + vp[1][3] * b.y + vp[2][3] * b.z + vp[3][3];
        constexpr float eps = 0.01f;
        if (wa < eps && wb < eps) return false; // both behind camera
        if (wa < eps) { float t = (eps - wa) / (wb - wa); a = Math::Mix(a, b, t); }
        if (wb < eps) { float t = (eps - wb) / (wa - wb); b = Math::Mix(b, a, t); }
        return true;
    }

    void ViewportOverlays::DrawClippedLine(ImDrawList* drawList, const EditorCamera& camera,
                                           const Vec3& worldA, const Vec3& worldB,
                                           ImU32 color, float thickness)
    {
        Vec3 a = worldA, b = worldB;
        if (!ClipLineToNearPlane(camera, a, b)) return;
        drawList->AddLine(ProjectToScreen(camera, a), ProjectToScreen(camera, b), color, thickness);
    }

    // ── Bone Debug ──

    void ViewportOverlays::DrawBoneDebug(const EditorCamera& camera, Entity selected)
    {
        if (!Editor::GetSettings().showBoneDebug) return;
        if (!selected || !selected.IsValid()) return;

        // Find the entity that owns Animation — selected entity or its parent
        Entity animEntity = selected;
        if (!animEntity.HasComponent<Animation>() && animEntity.HasParent()) {
            Entity parent = animEntity.GetParent();
            if (parent && parent.HasComponent<Animation>())
                animEntity = parent;
        }
        if (!animEntity.HasComponent<Animation>()) return;
        if (!animEntity.HasComponent<WorldTransform>()) return;

        auto& anim = animEntity.GetComponent<Animation>();
        auto& worldTransform = animEntity.GetComponent<WorldTransform>();

        if (anim.GlobalBoneTransforms.empty()) return;

        auto model = AssetManager::GetAsset<Model>(anim.ModelUUID);
        if (!model) return;
        const auto& skeleton = model->GetSkeleton();
        if (skeleton.IsEmpty()) return;

        ImDrawList* drawList = ImGui::GetWindowDrawList();
        const ImVec2* bounds = m_Viewport.GetBounds();
        drawList->PushClipRect(bounds[0], bounds[1], true);

        u32 boneCount = skeleton.BoneCount();
        u32 transformCount = (u32)anim.GlobalBoneTransforms.size();
        u32 count = std::min(boneCount, transformCount);

        std::vector<ImVec2> screenPositions(count);
        std::vector<bool> visible(count, false);

        for (u32 i = 0; i < count; i++) {
            Vec3 boneLocalPos = Vec3(anim.GlobalBoneTransforms[i][3]);
            Vec3 boneWorldPos = Vec3(worldTransform.Matrix * Vec4(boneLocalPos, 1.0f));
            screenPositions[i] = ProjectToScreen(camera, boneWorldPos);
            visible[i] = IsInViewport(screenPositions[i]);
        }

        ImU32 lineColor  = IM_COL32(0, 255, 128, 200);
        ImU32 jointColor = IM_COL32(255, 255, 0, 255);

        for (u32 i = 0; i < count; i++) {
            i32 parentIdx = skeleton.Bones[i].ParentIndex;
            if (parentIdx >= 0 && parentIdx < (i32)count) {
                if (visible[i] || visible[parentIdx]) {
                    drawList->AddLine(screenPositions[parentIdx], screenPositions[i],
                                      lineColor, 2.0f);
                }
            }
            if (visible[i]) {
                drawList->AddCircleFilled(screenPositions[i], 3.0f, jointColor);
            }
        }

        drawList->PopClipRect();
    }

    // ── Light Gizmos ──

    void ViewportOverlays::DrawLights(const std::shared_ptr<Scene>& scene,
                                      const EditorCamera& camera, Entity selected)
    {
        if (!Editor::GetSettings().showLightGizmos) return;
        if (!scene) return;

        auto& registry = scene->Registry();
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        const ImVec2* bounds = m_Viewport.GetBounds();
        drawList->PushClipRect(bounds[0], bounds[1], true);

        const bool isHovered = m_Viewport.IsHovered();
        const bool hasValidSelection = selected && selected.IsValid();

        // --- Directional Lights ---
        {
            auto view = registry.view<WorldTransform, DirectionalLight>();
            for (auto entity : view) {
                auto& wt = view.get<WorldTransform>(entity);
                auto& dl = view.get<DirectionalLight>(entity);

                Vec3 pos = Vec3(wt.Matrix[3]);
                Vec3 dir = Math::Normalize(-Vec3(wt.Matrix[2]));
                ImU32 color = LightColorToImU32(dl.Color);

                // Icon (only if in front of camera)
                ImVec2 screenPos = ProjectToScreen(camera, pos);
                if (screenPos.x >= 0.0f)
                    m_Gizmo.DrawGizmoIcon(drawList, screenPos, ICON_FA_SUN, color, entity, isHovered, hasValidSelection);

                // Direction arrow — only when selected
                Entity e(entity, scene.get());
                if (!EditorSelection::IsSelected(e)) continue;

                // Constant screen-size: compute pixel-per-unit at this depth
                Vec3 startPos = pos + dir * 1.5f; // offset to avoid icon overlap
                ImVec2 startScreen = ProjectToScreen(camera, startPos);
                ImVec2 unitScreen  = ProjectToScreen(camera, startPos + dir);
                float pxPerUnit = sqrtf((unitScreen.x - startScreen.x) * (unitScreen.x - startScreen.x)
                                      + (unitScreen.y - startScreen.y) * (unitScreen.y - startScreen.y));
                if (pxPerUnit < 0.001f) continue;
                constexpr float desiredPx = 80.0f;
                float worldLen = Math::Clamp(desiredPx / pxPerUnit, 0.5f, 50.0f);

                Vec3 endWorld = startPos + dir * worldLen;
                ImVec2 endScreen = ProjectToScreen(camera, endWorld);

                DrawClippedLine(drawList, camera, startPos, endWorld, color, 2.0f);

                // Arrowhead (in screen space)
                ImVec2 dir2D = { endScreen.x - startScreen.x, endScreen.y - startScreen.y };
                float len = sqrtf(dir2D.x * dir2D.x + dir2D.y * dir2D.y);
                if (len > 1.0f) {
                    ImVec2 norm = { dir2D.x / len, dir2D.y / len };
                    ImVec2 perp = { -norm.y, norm.x };
                    constexpr float arrowSize = 10.0f;
                    ImVec2 a1 = { endScreen.x - norm.x * arrowSize + perp.x * arrowSize * 0.5f,
                                  endScreen.y - norm.y * arrowSize + perp.y * arrowSize * 0.5f };
                    ImVec2 a2 = { endScreen.x - norm.x * arrowSize - perp.x * arrowSize * 0.5f,
                                  endScreen.y - norm.y * arrowSize - perp.y * arrowSize * 0.5f };
                    drawList->AddTriangleFilled(endScreen, a1, a2, color);
                }
            }
        }

        // --- Point Lights ---
        {
            auto view = registry.view<WorldTransform, PointLight>();
            for (auto entity : view) {
                auto& wt = view.get<WorldTransform>(entity);
                auto& pl = view.get<PointLight>(entity);

                Vec3 center = Vec3(wt.Matrix[3]);
                ImU32 iconColor = LightColorToImU32(pl.Color);

                // Icon (only if in front of camera)
                ImVec2 screenCenter = ProjectToScreen(camera, center);
                if (screenCenter.x >= 0.0f)
                    m_Gizmo.DrawGizmoIcon(drawList, screenCenter, ICON_FA_LIGHTBULB, iconColor, entity, isHovered, hasValidSelection);

                // Range circles — only when selected
                Entity e(entity, scene.get());
                if (!EditorSelection::IsSelected(e)) continue;

                float radius = pl.Range;
                ImU32 color = LightColorToImU32(pl.Color, 0.6f);

                constexpr int segments = 32;
                constexpr float twoPi = Math::TwoPi<float>;

                for (int plane = 0; plane < 3; plane++) {
                    Vec3 prevWorld;
                    for (int i = 0; i <= segments; i++) {
                        float angle = (float)i / (float)segments * twoPi;
                        float c = cosf(angle) * radius;
                        float s = sinf(angle) * radius;
                        Vec3 offset;
                        if (plane == 0)      offset = Vec3(c, s, 0.0f);
                        else if (plane == 1) offset = Vec3(c, 0.0f, s);
                        else                 offset = Vec3(0.0f, c, s);

                        Vec3 worldPt = center + offset;
                        if (i > 0)
                            DrawClippedLine(drawList, camera, prevWorld, worldPt, color, 1.5f);
                        prevWorld = worldPt;
                    }
                }
            }
        }

        // --- Spot Lights ---
        // Billboard icon only (for viewport selection); the cone wireframe lands with the debug-draw refactor.
        {
            auto view = registry.view<WorldTransform, SpotLight>();
            for (auto entity : view) {
                auto& wt = view.get<WorldTransform>(entity);
                auto& sl = view.get<SpotLight>(entity);

                Vec3   center      = Vec3(wt.Matrix[3]);
                ImU32  iconColor   = LightColorToImU32(sl.Color);
                ImVec2 screenCenter = ProjectToScreen(camera, center);
                if (screenCenter.x >= 0.0f)
                    m_Gizmo.DrawGizmoIcon(drawList, screenCenter, ICON_FA_SATELLITE_DISH, iconColor, entity, isHovered, hasValidSelection);
            }
        }

        drawList->PopClipRect();
    }

    // ── Camera Gizmos ──

    void ViewportOverlays::DrawCameras(const std::shared_ptr<Scene>& scene,
                                       const EditorCamera& camera, Entity selected)
    {
        if (!Editor::GetSettings().showCameraGizmos) return;
        if (!scene) return;

        auto& registry = scene->Registry();
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        const ImVec2* bounds = m_Viewport.GetBounds();
        drawList->PushClipRect(bounds[0], bounds[1], true);

        const bool isHovered = m_Viewport.IsHovered();
        const bool hasValidSelection = selected && selected.IsValid();

        auto view = registry.view<WorldTransform, Camera>();
        for (auto entity : view) {
            auto& wt  = view.get<WorldTransform>(entity);
            auto& cam = view.get<Camera>(entity);

            Vec3 pos = Vec3(wt.Matrix[3]);

            // Icon (only if in front of camera)
            ImVec2 screenPos = ProjectToScreen(camera, pos);
            if (screenPos.x >= 0.0f)
                m_Gizmo.DrawGizmoIcon(drawList, screenPos, ICON_FA_VIDEO, EditorColors::GizmoCamera, entity, isHovered, hasValidSelection);

            // Compute frustum corners in camera local space (looking along -Z)
            float visualFar = Math::Min(cam.FarClip, 1000.0f);
            Vec3 nearCorners[4], farCorners[4];

            if (cam.Projection == Camera::ProjectionType::Perspective) {
                float fovRad = Math::Radians(cam.VerticalFOV);
                float nearH = tanf(fovRad * 0.5f) * cam.NearClip;
                float nearW = nearH * cam.AspectRatio;
                float farH  = tanf(fovRad * 0.5f) * visualFar;
                float farW  = farH * cam.AspectRatio;

                nearCorners[0] = Vec3(-nearW,  nearH, -cam.NearClip);
                nearCorners[1] = Vec3( nearW,  nearH, -cam.NearClip);
                nearCorners[2] = Vec3( nearW, -nearH, -cam.NearClip);
                nearCorners[3] = Vec3(-nearW, -nearH, -cam.NearClip);

                farCorners[0] = Vec3(-farW,  farH, -visualFar);
                farCorners[1] = Vec3( farW,  farH, -visualFar);
                farCorners[2] = Vec3( farW, -farH, -visualFar);
                farCorners[3] = Vec3(-farW, -farH, -visualFar);
            }
            else {
                float halfH = cam.OrthographicSize * 0.5f;
                float halfW = halfH * cam.AspectRatio;

                nearCorners[0] = Vec3(-halfW,  halfH, -cam.OrthographicNear);
                nearCorners[1] = Vec3( halfW,  halfH, -cam.OrthographicNear);
                nearCorners[2] = Vec3( halfW, -halfH, -cam.OrthographicNear);
                nearCorners[3] = Vec3(-halfW, -halfH, -cam.OrthographicNear);

                float orthoFar = Math::Min(cam.OrthographicFar, 50.0f);
                farCorners[0] = Vec3(-halfW,  halfH, -orthoFar);
                farCorners[1] = Vec3( halfW,  halfH, -orthoFar);
                farCorners[2] = Vec3( halfW, -halfH, -orthoFar);
                farCorners[3] = Vec3(-halfW, -halfH, -orthoFar);
            }

            // Transform to world space
            Vec3 nearWorld[4], farWorld[4];
            for (int i = 0; i < 4; i++) {
                nearWorld[i] = Vec3(wt.Matrix * Vec4(nearCorners[i], 1.0f));
                farWorld[i]  = Vec3(wt.Matrix * Vec4(farCorners[i], 1.0f));
            }

            ImU32 color = EditorColors::GizmoCamera;

            // Near plane quad
            for (int i = 0; i < 4; i++)
                DrawClippedLine(drawList, camera, nearWorld[i], nearWorld[(i + 1) % 4], color, 1.5f);
            // Far plane quad
            for (int i = 0; i < 4; i++)
                DrawClippedLine(drawList, camera, farWorld[i], farWorld[(i + 1) % 4], color, 1.5f);
            // Connecting edges
            for (int i = 0; i < 4; i++)
                DrawClippedLine(drawList, camera, nearWorld[i], farWorld[i], color, 1.0f);
        }

        drawList->PopClipRect();
    }

    // ── AABB Gizmos ──

    void ViewportOverlays::DrawAABBs(const std::shared_ptr<Scene>& scene,
                                     const EditorCamera& camera)
    {
        if (!Editor::GetSettings().showAABBGizmos) return;
        if (!scene) return;

        auto& registry = scene->Registry();
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        const ImVec2* bounds = m_Viewport.GetBounds();
        drawList->PushClipRect(bounds[0], bounds[1], true);

        constexpr int edges[12][2] = {
            {0,1},{1,2},{2,3},{3,0},
            {4,5},{5,6},{6,7},{7,4},
            {0,4},{1,5},{2,6},{3,7}
        };

        auto view = registry.view<WorldTransform, MeshRenderer>();
        for (auto entity : view) {
            auto& wt = view.get<WorldTransform>(entity);
            auto& mr = view.get<MeshRenderer>(entity);

            // Check for animated AABB first
            AABB aabb;
            bool worldSpace = false;
            if (registry.all_of<Animation>(entity)) {
                auto& anim = registry.get<Animation>(entity);
                if (anim.AnimatedAABB.IsValid()) {
                    aabb = anim.AnimatedAABB;
                    worldSpace = true;
                }
            }

            // Fall back to bind-pose AABB from model
            if (!worldSpace) {
                auto model = AssetManager::GetAsset<Model>(mr.ModelUUID);
                if (!model) continue;
                auto& meshes = model->GetMeshesData();
                if (mr.MeshIndex >= meshes.size()) continue;
                aabb = meshes[mr.MeshIndex].BindPoseAABB;
                if (!aabb.IsValid()) continue;
            }

            // Compute 8 world-space corners
            Vec3 mn = aabb.Min, mx = aabb.Max;
            Vec3 localCorners[8] = {
                {mn.x, mn.y, mn.z}, {mx.x, mn.y, mn.z},
                {mx.x, mx.y, mn.z}, {mn.x, mx.y, mn.z},
                {mn.x, mn.y, mx.z}, {mx.x, mn.y, mx.z},
                {mx.x, mx.y, mx.z}, {mn.x, mx.y, mx.z},
            };

            Vec3 worldCorners[8];
            for (int i = 0; i < 8; i++)
                worldCorners[i] = worldSpace ? localCorners[i] : Vec3(wt.Matrix * Vec4(localCorners[i], 1.0f));

            Entity e(entity, scene.get());
            ImU32 color = EditorSelection::IsSelected(e)
                ? EditorColors::GizmoAABBSelected
                : EditorColors::GizmoAABB;

            for (auto& [a, b] : edges)
                DrawClippedLine(drawList, camera, worldCorners[a], worldCorners[b], color, 1.0f);
        }

        drawList->PopClipRect();
    }
}
