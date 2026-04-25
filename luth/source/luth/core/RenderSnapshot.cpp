#include "luthpch.h"
#include "luth/core/RenderSnapshot.h"

#include "luth/core/diagnostics/Log.h"
#include "luth/core/diagnostics/Profiler.h"
#include "luth/memory/LinearAllocator.h"
#include "luth/renderer/resources/Model.h"
#include "luth/resources/AssetManager.h"
#include "luth/scene/Components.h"
#include "luth/scene/Scene.h"

#include <new>

namespace Luth
{
    void CaptureSnapshot(Scene& scene, Memory::LinearAllocator& mem, RenderSnapshot& out)
    {
        LH_PROFILE_FUNCTION();

        out.Clear();

        auto& registry = scene.Registry();

        // ── Mesh draws ──
        // One row per (WorldTransform + MeshRenderer) entity with a valid model + mesh index.
        // boneOffset is pre-resolved here from the entity's Animation or its parent's, so the
        // render stage never walks the registry to find one (lookup used to be duplicated in
        // DrawListBuilder + GPUObjectBuffers).
        {
            auto view = registry.view<Component::WorldTransform, Component::MeshRenderer>();
            const size_t maxMeshes = view.size_hint();
            auto* meshRows = maxMeshes > 0
                ? static_cast<MeshDrawSnapshot*>(
                      mem.Allocate(maxMeshes * sizeof(MeshDrawSnapshot), alignof(MeshDrawSnapshot)))
                : nullptr;
            size_t count = 0;

            for (auto [entity, wt, mr] : view.each())
            {
                if (!mr.ModelUUID.IsValid()) continue;
                auto model = AssetManager::GetAsset<Model>(mr.ModelUUID);
                if (!model) continue;
                const auto& meshesData = model->GetMeshesData();
                if (mr.MeshIndex >= (u32)meshesData.size()) continue;

                MeshDrawSnapshot* dst = new (meshRows + count) MeshDrawSnapshot();
                dst->worldMatrix  = wt.Matrix;
                dst->modelUUID    = mr.ModelUUID;
                dst->materialUUID = mr.MaterialUUID;
                dst->meshIndex    = mr.MeshIndex;
                dst->isSkinned    = meshesData[mr.MeshIndex].IsSkinned;
                dst->entity       = static_cast<u32>(entity);

                // Animation lives on this entity OR its direct parent. Without the parent
                // fallback, child meshes read boneOffset=0 and render a frozen pose.
                if (dst->isSkinned)
                {
                    entt::entity animEntity = entt::null;
                    if (registry.all_of<Component::Animation>(entity))
                        animEntity = entity;
                    else if (registry.all_of<Component::Parent>(entity))
                    {
                        auto parentEnt = static_cast<entt::entity>(
                            registry.get<Component::Parent>(entity).Value);
                        if (registry.valid(parentEnt) && registry.all_of<Component::Animation>(parentEnt))
                            animEntity = parentEnt;
                    }
                    if (animEntity != entt::null)
                    {
                        const auto& anim = registry.get<Component::Animation>(animEntity);
                        if (anim.BufferAllocated)
                            dst->boneOffset = anim.BoneBufferOffset;
                    }
                }

                ++count;
            }

            out.meshes = std::span<const MeshDrawSnapshot>(meshRows, count);
        }

        // ── Directional light (first-wins) ──
        {
            auto view = registry.view<Component::WorldTransform, Component::DirectionalLight>();
            for (auto [entity, wt, dl] : view.each())
            {
                out.directionalLight.present                 = true;
                out.directionalLight.direction              = Math::Normalize(-Vec3(wt.Matrix[2]));
                out.directionalLight.color                  = dl.Color;
                out.directionalLight.intensity              = dl.Intensity;
                out.directionalLight.castShadows            = dl.CastShadows;
                out.directionalLight.shadowBias             = Vec4(dl.ShadowBias[0], dl.ShadowBias[1],
                                                                   dl.ShadowBias[2], dl.ShadowBias[3]);
                out.directionalLight.shadowNormalBias       = Vec4(dl.ShadowNormalBias[0], dl.ShadowNormalBias[1],
                                                                   dl.ShadowNormalBias[2], dl.ShadowNormalBias[3]);
                out.directionalLight.splitLambda            = Math::Clamp(dl.SplitLambda, 0.0f, 1.0f);
                out.directionalLight.shadowDistance         = dl.ShadowDistance;
                out.directionalLight.stabilizeCascades      = dl.StabilizeCascades;
                out.directionalLight.cascadeBlendWidth      = Math::Clamp(dl.CascadeBlendWidth, 0.0f, 1.0f);
                out.directionalLight.debugVisualizeCascades = dl.DebugVisualizeCascades;
                break;
            }
        }

        // ── Point lights (cap 64, matching LightUniforms) ──
        {
            constexpr size_t k_MaxPointLights = 64;
            auto view = registry.view<Component::WorldTransform, Component::PointLight>();
            const size_t maxCount = std::min<size_t>(view.size_hint(), k_MaxPointLights);
            auto* lightRows = maxCount > 0
                ? static_cast<PointLightSnapshot*>(
                      mem.Allocate(maxCount * sizeof(PointLightSnapshot), alignof(PointLightSnapshot)))
                : nullptr;
            size_t count = 0;

            for (auto [entity, wt, pl] : view.each())
            {
                if (count >= k_MaxPointLights) break;
                PointLightSnapshot* dst = new (lightRows + count) PointLightSnapshot();
                dst->position  = Vec3(wt.Matrix[3]);
                dst->color     = pl.Color;
                dst->intensity = pl.Intensity;
                dst->range     = pl.Range;
                ++count;
            }

            out.pointLights = std::span<const PointLightSnapshot>(lightRows, count);
        }

        // tagsByEntity: empty in S2 — Frame Debugger capture path is wired in S5.

        LH_CORE_TRACE("CaptureSnapshot: meshes={} pointLights={} dirLight={}",
                      out.meshes.size(), out.pointLights.size(),
                      out.directionalLight.present);
    }
}
