#include "luthpch.h"
#include "luth/core/RenderSnapshot.h"

#include "luth/core/diagnostics/Profiler.h"
#include "luth/memory/LinearAllocator.h"
#include "luth/renderer/RenderPipeline.h"
#include "luth/renderer/Renderer.h"
#include "luth/renderer/material/Material.h"
#include "luth/renderer/material/MaterialSystem.h"
#include "luth/renderer/resources/Model.h"
#include "luth/resources/AssetManager.h"
#include "luth/scene/Components.h"
#include "luth/scene/Scene.h"
#include "luth/scene/systems/RenderingSystem.h"
#include "luth/scene/systems/SystemRegistry.h"

#include <cstring>
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

        // ── Frame Debugger entity tags ──
        // Indexed by entt::to_entity(handle) (dense index, version stripped).
        // Strings are copied into LogicMemory so the snapshot stays valid across
        // game-stage mutations once stages run concurrently.
        {
            auto view = registry.view<Component::Tag>();
            u32 maxIdx = 0;
            bool any = false;
            for (auto e : view)
            {
                u32 idx = entt::to_entity(e);
                if (idx > maxIdx) maxIdx = idx;
                any = true;
            }

            if (any)
            {
                const size_t arrSize = static_cast<size_t>(maxIdx) + 1;
                auto** arr = static_cast<const char**>(
                    mem.Allocate(arrSize * sizeof(const char*), alignof(const char*)));
                std::memset(arr, 0, arrSize * sizeof(const char*));

                for (auto [e, tag] : view.each())
                {
                    const std::string& str = tag.Value;
                    char* copy = static_cast<char*>(mem.Allocate(str.size() + 1, alignof(char)));
                    std::memcpy(copy, str.c_str(), str.size() + 1);
                    arr[entt::to_entity(e)] = copy;
                }

                out.tagsByEntity = std::span<const char* const>(arr, arrSize);
            }
        }

        // ── Material registration + dirty flush (game stage) ──
        // Mirrors the legacy loop from RenderingSystem::Update. Walks the registry
        // once more so even entities skipped by the mesh capture (no model loaded
        // yet) still hold their material refs alive, matching prior behavior.
        // MaterialSystem::Update flushes any dirty material UBO writes; runs here
        // so its m_Lock stays stage-isolated to the game thread once stages
        // overlap (asserted in S9).
        if (auto* rs = SystemRegistry::GetSystem<RenderingSystem>())
        {
            RenderPipeline& pipeline = rs->GetPipeline();
            auto matView = registry.view<Component::WorldTransform, Component::MeshRenderer>();
            for (auto [entity, wt, mr] : matView.each())
            {
                if (mr.ModelUUID.IsValid())
                {
                    if (auto model = AssetManager::GetAsset<Model>(mr.ModelUUID))
                        scene.HoldAsset(mr.ModelUUID, model);
                }
                if (!mr.MaterialUUID.IsValid()) continue;
                if (auto material = AssetManager::GetAsset<Material>(mr.MaterialUUID))
                {
                    scene.HoldAsset(mr.MaterialUUID, material);
                    pipeline.EnsureMaterialRegistered(material);
                }
            }
            MaterialSystem::Update(VK_NULL_HANDLE);
        }
    }
}
