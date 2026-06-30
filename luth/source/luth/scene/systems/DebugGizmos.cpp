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
#include "luth/scene/components/Wind.h"
#include "luth/renderer/settings/WindSettings.h"

#include <cmath>

namespace Luth::Gizmos
{
    using namespace Component;

    namespace
    {
        // Light wireframe base alphas (the light's own RGB, dimmed); scaled by the scope alpha.
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

        // Per-category visibility scope, mirroring the physics-debug toggle model.
        struct Scope { bool selected, all; };
        bool  ScopeDraws(Scope s, bool isSel) { return s.all || (s.selected && isSel); }
        float ScopeAlpha(Scope s, bool isSel, float dim) { return (s.all && !isSel) ? dim : 1.0f; }

        void DrawLights(Scene& scene, const CameraParams& cam)
        {
            auto& reg = scene.Registry();
            const Scope sc{ cam.lightsSelected, cam.lightsAll };
            const float dim = cam.gizmoAlphaUnselected;

            auto dirView = reg.view<WorldTransform, DirectionalLight>();
            for (auto e : dirView)
            {
                const bool isSel = IsSelected(cam.selectedEntities, e);
                if (!ScopeDraws(sc, isSel)) continue;
                const float a = ScopeAlpha(sc, isSel, dim);
                auto& wt = dirView.get<WorldTransform>(e);
                auto& dl = dirView.get<DirectionalLight>(e);
                const Vec3 pos = Vec3(wt.Matrix[3]);
                const Vec3 dir = Math::Normalize(-Vec3(wt.Matrix[2]));   // beam = entity -Z
                const float len = Math::Clamp(kArrowK * Math::Length(pos - cam.position), 0.5f, 50.0f);
                const Vec3 start = pos + dir * 1.5f;                     // offset clear of the icon
                DebugDraw::Arrow(start, start + dir * len, Pack(dl.Color, kArrowAlpha * a), len * 0.2f);
            }

            auto pointView = reg.view<WorldTransform, PointLight>();
            for (auto e : pointView)
            {
                const bool isSel = IsSelected(cam.selectedEntities, e);
                if (!ScopeDraws(sc, isSel)) continue;
                const float a = ScopeAlpha(sc, isSel, dim);
                auto& wt = pointView.get<WorldTransform>(e);
                auto& pl = pointView.get<PointLight>(e);
                DebugDraw::WireSphere(Vec3(wt.Matrix[3]), pl.Range, Pack(pl.Color, kRangeAlpha * a));
            }

            auto spotView = reg.view<WorldTransform, SpotLight>();
            for (auto e : spotView)
            {
                const bool isSel = IsSelected(cam.selectedEntities, e);
                if (!ScopeDraws(sc, isSel)) continue;
                const float a = ScopeAlpha(sc, isSel, dim);
                auto& wt = spotView.get<WorldTransform>(e);
                auto& sl = spotView.get<SpotLight>(e);
                const Vec3 apex = Vec3(wt.Matrix[3]);
                const Vec3 dir  = Math::Normalize(-Vec3(wt.Matrix[2]));
                // Half-angle is measured from the axis; clamp under 90° so tan() stays finite.
                const float outerR = sl.Range * std::tan(Math::Radians(Math::Min(sl.OuterConeAngleDeg, 89.0f)));
                const float innerR = sl.Range * std::tan(Math::Radians(Math::Min(sl.InnerConeAngleDeg, 89.0f)));
                DebugDraw::WireCone(apex, dir, sl.Range, outerR, Pack(sl.Color, kConeOuterAlpha * a));
                DebugDraw::WireCone(apex, dir, sl.Range, innerR, Pack(sl.Color, kConeInnerAlpha * a));
            }
        }

        void DrawCameras(Scene& scene, const CameraParams& cam)
        {
            auto& reg = scene.Registry();
            const Scope sc{ cam.camerasSelected, cam.camerasAll };
            const float dim = cam.gizmoAlphaUnselected;
            auto view = reg.view<WorldTransform, Camera>();
            for (auto e : view)
            {
                const bool isSel = IsSelected(cam.selectedEntities, e);
                if (!ScopeDraws(sc, isSel)) continue;
                Vec4 colv = cam.gizmoCameraColor; colv.a *= ScopeAlpha(sc, isSel, dim);
                const u32 color = Pack(colv);
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
            const Scope sc{ cam.boundsSelected, cam.boundsAll };
            const float dim = cam.gizmoAlphaUnselected;
            auto view = reg.view<WorldTransform, MeshRenderer>();
            for (auto e : view)
            {
                const bool isSel = IsSelected(cam.selectedEntities, e);
                if (!ScopeDraws(sc, isSel)) continue;
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

                Vec4 colv = isSel ? cam.gizmoAABBSelectedColor : cam.gizmoAABBColor;
                colv.a *= ScopeAlpha(sc, isSel, dim);
                DebugDraw::WireBox(worldSpace ? Mat4(1.0f) : wt.Matrix, aabb.Min, aabb.Max, Pack(colv));
            }
        }

        // One skeleton's bone lines + joint crosses, alpha-scaled by aMul.
        void DrawSkeleton(const Animation& anim, const WorldTransform& wt, const CameraParams& cam, float aMul)
        {
            if (anim.GlobalBoneTransforms.empty()) return;
            auto model = AssetManager::GetAsset<Model>(anim.ModelUUID);
            if (!model) return;
            const auto& skeleton = model->GetSkeleton();
            if (skeleton.IsEmpty()) return;

            Vec4 lc = cam.gizmoBoneLineColor;  lc.a *= aMul;
            Vec4 jc = cam.gizmoBoneJointColor; jc.a *= aMul;
            const u32 lineColor  = Pack(lc);
            const u32 jointColor = Pack(jc);

            const u32 count = Math::Min<u32>((u32)skeleton.BoneCount(), (u32)anim.GlobalBoneTransforms.size());
            std::vector<Vec3> worldPos(count);
            for (u32 i = 0; i < count; ++i)
                worldPos[i] = Vec3(wt.Matrix * Vec4(Vec3(anim.GlobalBoneTransforms[i][3]), 1.0f));
            for (u32 i = 0; i < count; ++i)
            {
                const i32 parent = skeleton.Bones[i].ParentIndex;
                if (parent >= 0 && parent < (i32)count)
                    DebugDraw::Line(worldPos[parent], worldPos[i], lineColor);
                const float js = Math::Clamp(kJointK * Math::Length(worldPos[i] - cam.position), 0.01f, 1.0f);
                DebugDraw::Cross(worldPos[i], js, jointColor);
            }
        }

        void DrawBones(Scene& scene, const CameraParams& cam)
        {
            const Scope sc{ cam.bonesSelected, cam.bonesAll };
            auto& reg = scene.Registry();

            if (sc.all)
            {
                // Whole-scene skeletons — opt-in (default off): a many-character cost the selected path
                // avoids. Selection-vs-anim-owner mismatch only dims a child-selected rig, harmless here.
                auto view = reg.view<Animation, WorldTransform>();
                for (auto e : view)
                {
                    const bool isSel = IsSelected(cam.selectedEntities, e);
                    DrawSkeleton(view.get<Animation>(e), view.get<WorldTransform>(e), cam,
                                 ScopeAlpha(sc, isSel, cam.gizmoAlphaUnselected));
                }
                return;
            }
            if (!sc.selected) return;

            // Selected scope: bone data lives on the selected entity or its Animation-owning parent.
            for (const Entity& sel : cam.selectedEntities)
            {
                if (!sel || !sel.IsValid()) continue;
                Entity animEntity = sel;
                if (!animEntity.HasComponent<Animation>() && animEntity.HasParent())
                {
                    Entity parent = animEntity.GetParent();
                    if (parent && parent.HasComponent<Animation>()) animEntity = parent;
                }
                if (!animEntity.HasComponent<Animation>() || !animEntity.HasComponent<WorldTransform>()) continue;
                DrawSkeleton(animEntity.GetComponent<Animation>(), animEntity.GetComponent<WorldTransform>(), cam, 1.0f);
            }
        }

        void DrawFog(Scene& scene, const CameraParams& cam)
        {
            auto& reg = scene.Registry();
            const Scope sc{ cam.fogSelected, cam.fogAll };
            const float dim = cam.gizmoAlphaUnselected;
            auto view = reg.view<FogVolume, WorldTransform>();
            for (auto e : view)
            {
                const bool isSel = IsSelected(cam.selectedEntities, e);
                if (!ScopeDraws(sc, isSel)) continue;
                const auto& fog = view.get<FogVolume>(e);
                const auto& wt  = view.get<WorldTransform>(e);
                Vec4 colv = cam.gizmoFogColor; colv.a *= ScopeAlpha(sc, isSel, dim);
                const u32 color = Pack(colv);
                const Mat4 m = wt.Matrix * Math::Translate(Mat4(1.0f), fog.localOffset) * Math::ToMat4(fog.localRotation);
                if (fog.type == FogVolume::Type::Box)
                    DebugDraw::WireBox(m, -fog.halfExtents, fog.halfExtents, color);
                else
                    DebugDraw::WireSphere(Vec3(m[3]), fog.radius, color);
            }
        }

        void DrawWind(Scene& scene, const CameraParams& cam, const WindSettings& wind)
        {
            auto& reg = scene.Registry();
            const Scope sc{ cam.windSelected, cam.windAll };
            const float dim = cam.gizmoAlphaUnselected;

            auto view = reg.view<WorldTransform, Wind>();
            for (auto e : view)
            {
                const bool isSel = IsSelected(cam.selectedEntities, e);
                if (!ScopeDraws(sc, isSel)) continue;
                const auto& w = view.get<Wind>(e);
                if (!w.enabled) continue;
                const auto& wt = view.get<WorldTransform>(e);

                // Effective direction: per-entity override (world- or object-space) else the global field.
                Vec3 dir = wind.direction;
                if (w.useDirectionOverride)
                    dir = w.overrideIsWorldSpace ? w.directionOverride
                                                 : Vec3(wt.Matrix * Vec4(w.directionOverride, 0.0f));
                const float dlen = Math::Length(dir);
                if (dlen < 1e-5f) continue;
                dir *= 1.0f / dlen;

                // Length tracks effective strength, distance-proportional like the dir-light arrow.
                const Vec3  pos    = Vec3(wt.Matrix[3]);
                const float effStr = wind.enabled ? wind.strength * w.strengthMultiplier : 0.0f;
                const float base   = Math::Clamp(kArrowK * Math::Length(pos - cam.position), 0.5f, 50.0f);
                const float len    = base * Math::Clamp(0.25f + effStr, 0.25f, 3.0f);

                Vec4 colv = cam.gizmoWindColor; colv.a *= ScopeAlpha(sc, isSel, dim);
                DebugDraw::Arrow(pos, pos + dir * len, Pack(colv), len * 0.2f);
            }
        }
    }

    void Draw(Scene& scene, const CameraParams& cam, const WindSettings& wind)
    {
        LH_PROFILE_FUNCTION();
        if (cam.lightsSelected  || cam.lightsAll)  DrawLights(scene, cam);
        if (cam.camerasSelected || cam.camerasAll) DrawCameras(scene, cam);
        if (cam.boundsSelected  || cam.boundsAll)  DrawAABBs(scene, cam);
        if (cam.bonesSelected   || cam.bonesAll)   DrawBones(scene, cam);
        if (cam.fogSelected     || cam.fogAll)     DrawFog(scene, cam);
        if (cam.windSelected    || cam.windAll)    DrawWind(scene, cam, wind);
    }
}
