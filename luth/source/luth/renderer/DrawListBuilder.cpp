#include "luthpch.h"
#include "luth/renderer/DrawListBuilder.h"
#include "luth/core/RenderSnapshot.h"
#include "luth/renderer/draw/DrawList.h"
#include "luth/renderer/material/Material.h"
#include "luth/renderer/resources/Model.h"
#include "luth/resources/AssetManager.h"
#include "luth/renderer/backend/vulkan/UploadContext.h"

namespace Luth
{
    void DrawListBuilder::Build(const RenderSnapshot& snapshot,
                                const std::unordered_map<UUID, u32, UUIDHash>& materialSlotMap,
                                const std::unordered_map<entt::entity, u32>& entityToSSBOIndex,
                                DrawList& out)
    {
        LH_PROFILE_FUNCTION();

        out.Clear();

        // Sample the upload timeline once; gate each mesh below so a draw never references a VB/IB whose
        // async upload has not retired (the deferred BLAS build no longer waits on it). Progressive load.
        const u64 uploadDone = UploadContext::Get().CompletedUploadValue();

        for (const MeshDrawSnapshot& meshSnap : snapshot.meshes)
        {
            // Entities absent from entityToSSBOIndex were skipped by BuildGPUObjectBuffer (over
            // k_MaxGPUObjects, no GetMesh, etc.); drawing them via indirect would reference stale SSBO slots.
            entt::entity entity = static_cast<entt::entity>(meshSnap.entity);
            auto ssboIt = entityToSSBOIndex.find(entity);
            if (ssboIt == entityToSSBOIndex.end()) continue;

            auto model = AssetManager::GetAsset<Model>(meshSnap.modelUUID);
            if (!model) continue;
            auto mesh = model->GetMesh(meshSnap.meshIndex);
            if (!mesh) continue;

            // Skip until this mesh's VB/IB upload retires, so no draw reads a not-yet-resident buffer.
            auto vbuf = mesh->GetVertexBuffer();
            auto ibuf = mesh->GetIndexBuffer();
            const u64 upFence = std::max<u64>(vbuf ? vbuf->GetUploadFence() : 0, ibuf ? ibuf->GetUploadFence() : 0);
            if (upFence > uploadDone) continue;

            if (ibuf)
                out.visibleTriCount += ibuf->GetCount() / 3;

            DrawCommand dc;
            dc.modelMatrix    = meshSnap.worldMatrix;
            dc.materialSlot   = 0;
            dc.model          = model;
            dc.meshIndex      = meshSnap.meshIndex;
            dc.entity         = entity;
            dc.gpuObjectIndex = ssboIt->second;
            dc.entityIndex    = ssboIt->second + 1;

            Material::RenderMode mode = Material::RenderMode::Opaque;
            Material::CullMode   cullMode = Material::CullMode::Back;
            if (meshSnap.materialUUID.IsValid())
            {
                auto material = AssetManager::GetAsset<Material>(meshSnap.materialUUID);
                if (material)
                {
                    auto slotIt = materialSlotMap.find(material->Handle);
                    if (slotIt != materialSlotMap.end())
                        dc.materialSlot = slotIt->second;
                    mode     = material->GetRenderMode();
                    cullMode = material->GetCullMode();
                    // Variant 0 = stock decode in RT + transparent (unregistered or beyond the variant cap);
                    // leave the raster override unset too so every tier agrees on stock. invariant: raster==RT.
                    if (material->GetGraphVariant() != 0)
                        dc.fragShaderUUID = material->GetGraphShaderUUID();
                }
            }
            dc.cullMode = cullMode;

            if (meshSnap.isSkinned)
            {
                dc.isSkinned  = true;
                dc.boneOffset = meshSnap.boneOffset;
            }
            dc.isDeformable = meshSnap.isDeformable;
            dc.isDeformed   = dc.isSkinned || dc.isDeformable;

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
