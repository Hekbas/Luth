#include "luthpch.h"
#include "luth/renderer/DrawListBuilder.h"
#include "luth/renderer/draw/DrawList.h"
#include "luth/renderer/material/Material.h"
#include "luth/renderer/resources/Model.h"
#include "luth/resources/AssetManager.h"
#include "luth/scene/Components.h"

namespace Luth
{
    using namespace Component;

    void DrawListBuilder::Build(entt::registry& registry,
                                const std::unordered_map<UUID, u32, UUIDHash>& materialSlotMap,
                                const std::unordered_map<entt::entity, u32>& entityToSSBOIndex,
                                DrawList& out)
    {
        out.Clear();

        auto view = registry.view<WorldTransform, MeshRenderer>();
        for (auto [entity, worldTransform, meshRenderer] : view.each())
        {
            // Entities absent from entityToSSBOIndex were skipped by
            // BuildGPUObjectBuffer (invalid model, over k_MaxGPUObjects, etc.)
            // — drawing them via indirect would reference stale SSBO slots.
            auto ssboIt = entityToSSBOIndex.find(entity);
            if (ssboIt == entityToSSBOIndex.end()) continue;

            auto model = AssetManager::GetAsset<Model>(meshRenderer.ModelUUID);
            if (!model) continue;
            auto mesh = model->GetMesh(meshRenderer.MeshIndex);
            if (!mesh) continue;

            if (auto ib = mesh->GetIndexBuffer())
                out.visibleTriCount += ib->GetCount() / 3;

            DrawCommand dc;
            dc.modelMatrix    = worldTransform.Matrix;
            dc.materialSlot   = 0;
            dc.model          = model;
            dc.meshIndex      = meshRenderer.MeshIndex;
            dc.entity         = entity;
            dc.gpuObjectIndex = ssboIt->second;
            dc.entityIndex    = ssboIt->second + 1;

            Material::RenderMode mode = Material::RenderMode::Opaque;
            Material::CullMode   cullMode = Material::CullMode::Back;
            if (meshRenderer.MaterialUUID.IsValid())
            {
                auto material = AssetManager::GetAsset<Material>(meshRenderer.MaterialUUID);
                if (material)
                {
                    auto slotIt = materialSlotMap.find(material->Handle);
                    if (slotIt != materialSlotMap.end())
                        dc.materialSlot = slotIt->second;
                    mode     = material->GetRenderMode();
                    cullMode = material->GetCullMode();
                }
            }
            dc.cullMode = cullMode;

            // Per-mesh skinning: pull bone offset from Animation on this entity
            // or its direct parent (matches the SceneGraph lookup in GeometryPass).
            if (meshRenderer.MeshIndex < model->GetMeshesData().size()
                && model->GetMeshesData()[meshRenderer.MeshIndex].IsSkinned)
            {
                dc.isSkinned = true;
                entt::entity animEntity = entt::null;
                if (registry.any_of<Component::Animation>(entity))
                    animEntity = entity;
                else if (registry.any_of<Component::Parent>(entity))
                {
                    auto parentEnt = (entt::entity)registry.get<Component::Parent>(entity).Value;
                    if (registry.valid(parentEnt) && registry.any_of<Component::Animation>(parentEnt))
                        animEntity = parentEnt;
                }
                if (animEntity != entt::null)
                {
                    auto& anim = registry.get<Component::Animation>(animEntity);
                    if (anim.BufferAllocated)
                        dc.boneOffset = anim.BoneBufferOffset;
                }
            }

            switch (mode)
            {
                case Material::RenderMode::Cutout:      out.cutout.push_back(dc);      break;
                case Material::RenderMode::Transparent:
                case Material::RenderMode::Fade:        out.transparent.push_back(dc); break;
                default:                                out.opaque.push_back(dc);      break;
            }
        }
    }
}
