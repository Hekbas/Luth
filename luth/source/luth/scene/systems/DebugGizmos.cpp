#include "luthpch.h"

#include "luth/scene/systems/DebugGizmos.h"

#include "luth/core/DebugDraw.h"
#include "luth/core/diagnostics/Profiler.h"
#include "luth/renderer/CameraParams.h"
#include "luth/renderer/resources/Model.h"
#include "luth/resources/AssetManager.h"
#include "luth/scene/Components.h"
#include "luth/scene/Entity.h"
#include "luth/scene/Scene.h"
#include "luth/scene/components/FogVolume.h"

#include <cmath>

namespace Luth::Gizmos
{
    using namespace Component;

    namespace
    {
        // FogVolume gizmo color (R,G,B,A bytes) — the value the old render-stage producer used.
        constexpr u32 kFogGizmoColor = 0x80FFCC4D;

        // Selected-light wireframe alphas (the light's own RGB, dimmed).
        constexpr float kArrowAlpha     = 0.85f;
        constexpr float kRangeAlpha     = 0.60f;
        constexpr float kConeOuterAlpha = 0.60f;
        constexpr float kConeInnerAlpha = 0.35f;

        // Distance-proportional world size → roughly screen-constant on-screen. Tuned so the arrow
        // reads like the old ~80px ImGui overlay at a typical FOV; joints are a small fraction.
        constexpr float kArrowK = 0.085f;
        constexpr float kJointK = 0.006f;

        u32 Pack(const Vec4& c)
        {
            auto q = [](float v) -> u32 { return u32(Math::Clamp(v, 0.0f, 1.0f) * 255.0f) & 0xFFu; };
            return q(c.r) | (q(c.g) << 8) | (q(c.b) << 16) | (q(c.a) << 24);
        }
        u32 Pack(const Vec3& rgb, float a) { return Pack(Vec4(rgb, a)); }

        bool IsSelected(const std::vector<Entity>& sel, entt::entity handle)
        {
            for (const Entity& e : sel)
                if (static_cast<entt::entity>(e) == handle) return true;
            return false;
        }

        void DrawLights(Scene& scene, const CameraParams& cam)
        {
            auto& reg = scene.Registry();

            auto dirView = reg.view<WorldTransform, DirectionalLight>();
            for (auto e : dirView)
            {
                if (!IsSelected(cam.selectedEntities, e)) continue;
                auto& wt = dirView.get<WorldTransform>(e);
                auto& dl = dirView.get<DirectionalLight>(e);
                const Vec3 pos = Vec3(wt.Matrix[3]);
                const Vec3 dir = Math::Normalize(-Vec3(wt.Matrix[2]));   // beam = entity -Z
                const float len = Math::Clamp(kArrowK * Math::Length(pos - cam.position), 0.5f, 50.0f);
                const Vec3 start = pos + dir * 1.5f;                     // offset clear of the icon
                DebugDraw::Arrow(start, start + dir * len, Pack(dl.Color, kArrowAlpha), len * 0.2f);
            }

            auto pointView = reg.view<WorldTransform, PointLight>();
            for (auto e : pointView)
            {
                if (!IsSelected(cam.selectedEntities, e)) continue;
                auto& wt = pointView.get<WorldTransform>(e);
                auto& pl = pointView.get<PointLight>(e);
                DebugDraw::WireSphere(Vec3(wt.Matrix[3]), pl.Range, Pack(pl.Color, kRangeAlpha));
            }

            auto spotView = reg.view<WorldTransform, SpotLight>();
            for (auto e : spotView)
            {
                if (!IsSelected(cam.selectedEntities, e)) continue;
                auto& wt = spotView.get<WorldTransform>(e);
                auto& sl = spotView.get<SpotLight>(e);
                const Vec3 apex = Vec3(wt.Matrix[3]);
                const Vec3 dir  = Math::Normalize(-Vec3(wt.Matrix[2]));
                // Half-angle is measured from the axis; clamp under 90° so tan() stays finite.
                const float outerR = sl.Range * std::tan(Math::Radians(Math::Min(sl.OuterConeAngleDeg, 89.0f)));
                const float innerR = sl.Range * std::tan(Math::Radians(Math::Min(sl.InnerConeAngleDeg, 89.0f)));
                DebugDraw::WireCone(apex, dir, sl.Range, outerR, Pack(sl.Color, kConeOuterAlpha));
                DebugDraw::WireCone(apex, dir, sl.Range, innerR, Pack(sl.Color, kConeInnerAlpha));
            }
        }

        void DrawCameras(Scene& scene, const CameraParams& cam)
        {
            auto& reg = scene.Registry();
            const u32 color = Pack(cam.gizmoCameraColor);
            auto view = reg.view<WorldTransform, Camera>();
            for (auto e : view)
            {
                auto& wt = view.get<WorldTransform>(e);
                auto& c  = view.get<Camera>(e);
                const float visualFar = Math::Min(c.FarClip, 1000.0f);

                Vec3 nc[4], fc[4];
                if (c.Projection == Camera::ProjectionType::Perspective)
                {
                    const float t  = std::tan(Math::Radians(c.VerticalFOV) * 0.5f);
                    const float nh = t * c.NearClip, nw = nh * c.AspectRatio;
                    const float fh = t * visualFar,  fw = fh * c.AspectRatio;
                    nc[0] = Vec3(-nw, nh, -c.NearClip); nc[1] = Vec3(nw, nh, -c.NearClip);
                    nc[2] = Vec3(nw, -nh, -c.NearClip); nc[3] = Vec3(-nw, -nh, -c.NearClip);
                    fc[0] = Vec3(-fw, fh, -visualFar);  fc[1] = Vec3(fw, fh, -visualFar);
                    fc[2] = Vec3(fw, -fh, -visualFar);  fc[3] = Vec3(-fw, -fh, -visualFar);
                }
                else
                {
                    const float hh = c.OrthographicSize * 0.5f, hw = hh * c.AspectRatio;
                    const float of = Math::Min(c.OrthographicFar, 50.0f);
                    nc[0] = Vec3(-hw, hh, -c.OrthographicNear); nc[1] = Vec3(hw, hh, -c.OrthographicNear);
                    nc[2] = Vec3(hw, -hh, -c.OrthographicNear); nc[3] = Vec3(-hw, -hh, -c.OrthographicNear);
                    fc[0] = Vec3(-hw, hh, -of); fc[1] = Vec3(hw, hh, -of);
                    fc[2] = Vec3(hw, -hh, -of); fc[3] = Vec3(-hw, -hh, -of);
                }

                Vec3 corners[8];
                for (int i = 0; i < 4; ++i) corners[i]     = Vec3(wt.Matrix * Vec4(nc[i], 1.0f));
                for (int i = 0; i < 4; ++i) corners[4 + i] = Vec3(wt.Matrix * Vec4(fc[i], 1.0f));
                DebugDraw::WireFrustum(corners, color);
            }
        }

        void DrawAABBs(Scene& scene, const CameraParams& cam)
        {
            auto& reg = scene.Registry();
            const u32 colSel = Pack(cam.gizmoAABBSelectedColor);
            const u32 colDef = Pack(cam.gizmoAABBColor);
            auto view = reg.view<WorldTransform, MeshRenderer>();
            for (auto e : view)
            {
                auto& wt = view.get<WorldTransform>(e);
                auto& mr = view.get<MeshRenderer>(e);

                // Prefer the animated (world-space) AABB; fall back to the model's bind-pose AABB.
                AABB aabb;
                bool worldSpace = false;
                if (reg.all_of<Animation>(e))
                {
                    auto& anim = reg.get<Animation>(e);
                    if (anim.AnimatedAABB.IsValid()) { aabb = anim.AnimatedAABB; worldSpace = true; }
                }
                if (!worldSpace)
                {
                    auto model = AssetManager::GetAsset<Model>(mr.ModelUUID);
                    if (!model) continue;
                    auto& meshes = model->GetMeshesData();
                    if (mr.MeshIndex >= meshes.size()) continue;
                    aabb = meshes[mr.MeshIndex].BindPoseAABB;
                    if (!aabb.IsValid()) continue;
                }

                const u32 color = IsSelected(cam.selectedEntities, e) ? colSel : colDef;
                DebugDraw::WireBox(worldSpace ? Mat4(1.0f) : wt.Matrix, aabb.Min, aabb.Max, color);
            }
        }

        void DrawBones(Scene& scene, const CameraParams& cam)
        {
            (void)scene;  // walks the selection set, not a registry view
            const u32 lineColor  = Pack(cam.gizmoBoneLineColor);
            const u32 jointColor = Pack(cam.gizmoBoneJointColor);

            for (const Entity& sel : cam.selectedEntities)
            {
                if (!sel || !sel.IsValid()) continue;

                // Bone data lives on the selected entity or its Animation-owning parent.
                Entity animEntity = sel;
                if (!animEntity.HasComponent<Animation>() && animEntity.HasParent())
                {
                    Entity parent = animEntity.GetParent();
                    if (parent && parent.HasComponent<Animation>()) animEntity = parent;
                }
                if (!animEntity.HasComponent<Animation>() || !animEntity.HasComponent<WorldTransform>()) continue;

                auto& anim = animEntity.GetComponent<Animation>();
                auto& wt   = animEntity.GetComponent<WorldTransform>();
                if (anim.GlobalBoneTransforms.empty()) continue;

                auto model = AssetManager::GetAsset<Model>(anim.ModelUUID);
                if (!model) continue;
                const auto& skeleton = model->GetSkeleton();
                if (skeleton.IsEmpty()) continue;

                const u32 count = Math::Min<u32>((u32)skeleton.BoneCount(), (u32)anim.GlobalBoneTransforms.size());
                std::vector<Vec3> worldPos(count);
                for (u32 i = 0; i < count; ++i)
                {
                    const Vec3 local = Vec3(anim.GlobalBoneTransforms[i][3]);
                    worldPos[i] = Vec3(wt.Matrix * Vec4(local, 1.0f));
                }
                for (u32 i = 0; i < count; ++i)
                {
                    const i32 parent = skeleton.Bones[i].ParentIndex;
                    if (parent >= 0 && parent < (i32)count)
                        DebugDraw::Line(worldPos[parent], worldPos[i], lineColor);
                    const float js = Math::Clamp(kJointK * Math::Length(worldPos[i] - cam.position), 0.01f, 1.0f);
                    DebugDraw::Cross(worldPos[i], js, jointColor);
                }
            }
        }

        void DrawFog(Scene& scene)
        {
            auto& reg = scene.Registry();
            auto view = reg.view<FogVolume, WorldTransform>();
            for (auto e : view)
            {
                const auto& fog = view.get<FogVolume>(e);
                const auto& wt  = view.get<WorldTransform>(e);
                const Mat4 m = wt.Matrix * Math::Translate(Mat4(1.0f), fog.localOffset) * Math::ToMat4(fog.localRotation);
                if (fog.type == FogVolume::Type::Box)
                    DebugDraw::WireBox(m, -fog.halfExtents, fog.halfExtents, kFogGizmoColor);
                else
                    DebugDraw::WireSphere(Vec3(m[3]), fog.radius, kFogGizmoColor);
            }
        }
    }

    void Draw(Scene& scene, const CameraParams& cam)
    {
        LH_PROFILE_FUNCTION();
        if (cam.showLightGizmos)  DrawLights(scene, cam);
        if (cam.showCameraGizmos) DrawCameras(scene, cam);
        if (cam.showAABBGizmos)   DrawAABBs(scene, cam);
        if (cam.showBoneDebug)    DrawBones(scene, cam);
        DrawFog(scene);   // always on (no editor toggle), matching the old behavior
    }
}
