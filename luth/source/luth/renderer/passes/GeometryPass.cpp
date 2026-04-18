#include "luthpch.h"
#include "luth/scene/systems/RenderingSystem.h"
#include "luth/core/Profiler.h"
#include "luth/scene/Scene.h"
#include "luth/scene/Components.h"
#include "luth/renderer/Renderer.h"
#include "luth/renderer/MaterialSystem.h"
#include "luth/renderer/BoneMatrixBuffer.h"
#include "luth/renderer/backend/vulkan/VulkanBackend.h"
#include "luth/renderer/backend/vulkan/VulkanContext.h"
#include "luth/renderer/backend/vulkan/VulkanTexture.h"
#include "luth/renderer/backend/vulkan/VulkanBuffer.h"
#include "luth/renderer/Material.h"
#include "luth/renderer/Model.h"
#include "luth/resources/AssetManager.h"
#include "luth/renderer/ShaderCompiler.h"
#include "luth/renderer/ShaderLibrary.h"
#include "luth/renderer/backend/vulkan/VulkanShader.h"
#include <glm/gtc/matrix_transform.hpp>
#include <vma/vk_mem_alloc.h>

namespace Luth
{
    using namespace Component;

    GeometryOutput RenderingSystem::AddGeometryPass(
        RG::RenderGraph& rg, entt::registry& registry,
        const RG::ResourceHandle (&shadowHandles)[k_ShadowCascadeCount],
        RG::BufferHandle indirectBufferHandle,
        RG::ResourceHandle sceneDepth)
    {
        struct GeometryPassData {
            RG::ResourceHandle outputTex;
            RG::ResourceHandle entityIDTex;
            RG::ResourceHandle depthTex;
            RG::ResourceHandle shadowCascades[k_ShadowCascadeCount];
            RG::BufferHandle   indirectBuf;
        };

        GeometryOutput output;

        rg.AddPass<GeometryPassData>("GeometryPass",
            [&, sceneDepth](GeometryPassData& data, RG::RenderPassBuilder& builder)
            {
                RG::TextureDesc desc;
                desc.name   = "SceneColor";
                desc.width  = m_SceneColor->GetWidth();
                desc.height = m_SceneColor->GetHeight();
                desc.format = RG::TextureFormat::RGBA16_Float;

                auto vkTex = std::static_pointer_cast<VKTexture>(m_SceneColor);
                data.outputTex = rg.ImportResource(desc,
                    (void*)vkTex->GetImage(),
                    (void*)vkTex->GetImageView(),
                    RG::ResourceState::ShaderResource);

                // Entity ID buffer (R32_UINT)
                RG::TextureDesc idDesc;
                idDesc.name   = "EntityID";
                idDesc.width  = m_EntityIDBuffer->GetWidth();
                idDesc.height = m_EntityIDBuffer->GetHeight();
                idDesc.format = RG::TextureFormat::R32_Uint;

                auto vkID = std::static_pointer_cast<VKTexture>(m_EntityIDBuffer);
                data.entityIDTex = rg.ImportResource(idDesc,
                    (void*)vkID->GetImage(),
                    (void*)vkID->GetImageView(),
                    RG::ResourceState::Undefined);

                // Depth is produced by the earlier DepthPrepass — load it and keep
                // writing (cutouts still write their own depth; opaques pass LESS_EQUAL
                // against prepass values).
                data.depthTex  = builder.WriteDepth(sceneDepth,
                    VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE, {});
                data.outputTex = builder.Write(data.outputTex);

                VkClearValue idClear{};
                idClear.color.uint32[0] = 0;
                data.entityIDTex = builder.Write(data.entityIDTex,
                    VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, idClear);

                // Declare dependency on each cascade — each per-layer handle triggers its own
                // DEPTH→SHADER_READ barrier (baseArrayLayer=i, layerCount=1), collectively
                // transitioning all 4 layers before the fragment shader samples the array view.
                for (u32 i = 0; i < k_ShadowCascadeCount; ++i)
                {
                    if (shadowHandles[i].IsValid())
                        data.shadowCascades[i] = builder.Read(shadowHandles[i]);
                }

                // Declare indirect buffer read (triggers compute-write→indirect-read barrier)
                data.indirectBuf = builder.ReadIndirectBuffer(indirectBufferHandle);

                output.color    = data.outputTex;
                output.depth    = data.depthTex;
                output.entityID = data.entityIDTex;
            },
            [this, &registry](GeometryPassData& data, RG::RenderPassContext& ctx)
            {
                VkCommandBuffer cmd = ctx.commandBuffer;

                VkPolygonMode polyMode = (m_ShadeMode == ShadeMode::Wireframe) ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
                m_FrameDebugger.BeginCapturePass("GeometryPass", "SceneColor", false,
                    { "pbr", 0, VK_CULL_MODE_BACK_BIT, polyMode, false, true, true, false });

                UUID pbrUUID = ShaderLibrary::Get("pbr")->Handle;
                auto* opaquePipeline = m_GeoPipelineManager.GetOrCreate(
                    pbrUUID, Material::RenderMode::Opaque, Material::CullMode::Back, polyMode, m_PBRVertSpv, m_PBRFragSpv);
                if (!opaquePipeline) { m_FrameDebugger.EndCapturePass(); return; }
                VkPipelineLayout pipelineLayout = opaquePipeline->GetLayout();

                // Bind all 6 descriptor sets (Set 5 = GPUObjectData SSBO)
                VkDescriptorSet bindlessSet = VulkanContext::Get().GetBindlessSet().GetSet();
                VkDescriptorSet sets[] = {
                    m_GlobalDescriptorSet,
                    bindlessSet,
                    MaterialSystem::GetDescriptorSet(),
                    m_LightDescSet,
                    BoneMatrixBuffer::GetDescriptorSet(),
                    m_ObjectSSBODescSet
                };
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    pipelineLayout, 0, 6, sets, 0, nullptr);

                RG::RenderGraph::ResourceNode* res = (RG::RenderGraph::ResourceNode*)ctx.GetResource(data.outputTex);

                VkViewport viewport{};
                viewport.width    = (float)res->desc.width;
                viewport.height   = (float)res->desc.height;
                viewport.maxDepth = 1.0f;
                vkCmdSetViewport(cmd, 0, 1, &viewport);

                VkRect2D scissor{};
                scissor.extent = { res->desc.width, res->desc.height };
                vkCmdSetScissor(cmd, 0, 1, &scissor);

                // Collect draw commands per render mode (reuse member vectors to avoid per-frame heap alloc)
                m_OpaqueDraws.clear();
                m_CutoutDraws.clear();
                m_TransparentDraws.clear();
                m_VisibleTriCount = 0;

                // m_EntityLookup is now built in BuildGPUObjectBuffer (called before this pass).
                // Populate DrawCommands using m_EntityToSSBOIndex for SSBO index lookup.
                auto view = registry.view<WorldTransform, MeshRenderer>();
                for (auto [entity, worldTransform, meshRenderer] : view.each())
                {
                    auto model = AssetManager::GetAsset<Model>(meshRenderer.ModelUUID);
                    if (!model) continue;
                    auto mesh = model->GetMesh(meshRenderer.MeshIndex);
                    if (!mesh) continue;

                    if (auto ib = mesh->GetIndexBuffer())
                        m_VisibleTriCount += ib->GetCount() / 3;

                    DrawCommand dc;
                    dc.modelMatrix  = worldTransform.Matrix;
                    dc.materialSlot = 0;
                    dc.model        = model;
                    dc.meshIndex    = meshRenderer.MeshIndex;

                    // Look up SSBO index — dc.entityIndex is 1-indexed to match m_EntityLookup
                    auto it = m_EntityToSSBOIndex.find(entity);
                    if (it != m_EntityToSSBOIndex.end())
                    {
                        dc.gpuObjectIndex = it->second;
                        dc.entityIndex    = it->second + 1;
                    }

                    Material::RenderMode mode = Material::RenderMode::Opaque;
                    Material::CullMode cullMode = Material::CullMode::Back;

                    if (meshRenderer.MaterialUUID.IsValid())
                    {
                        auto material = AssetManager::GetAsset<Material>(meshRenderer.MaterialUUID);
                        if (material)
                        {
                            auto slotIt = m_MaterialSlotMap.find(material->Handle);
                            if (slotIt != m_MaterialSlotMap.end())
                                dc.materialSlot = slotIt->second;
                            mode = material->GetRenderMode();
                            cullMode = material->GetCullMode();
                        }
                    }
                    dc.cullMode = cullMode;

                    // Detect per-mesh skinning
                    if (meshRenderer.MeshIndex < model->GetMeshesData().size()
                        && model->GetMeshesData()[meshRenderer.MeshIndex].IsSkinned)
                    {
                        dc.isSkinned = true;
                        // Find Animation on this entity or parent
                        entt::entity animEntity = entt::null;
                        if (registry.any_of<Component::Animation>(entity))
                            animEntity = entity;
                        else if (registry.any_of<Component::Parent>(entity)) {
                            auto parentEnt = (entt::entity)registry.get<Component::Parent>(entity).Value;
                            if (registry.valid(parentEnt) && registry.any_of<Component::Animation>(parentEnt))
                                animEntity = parentEnt;
                        }
                        if (animEntity != entt::null) {
                            auto& anim = registry.get<Component::Animation>(animEntity);
                            if (anim.BufferAllocated)
                                dc.boneOffset = anim.BoneBufferOffset;
                        }
                    }

                    switch (mode)
                    {
                        case Material::RenderMode::Cutout:      m_CutoutDraws.push_back(dc);      break;
                        case Material::RenderMode::Transparent:
                        case Material::RenderMode::Fade:        m_TransparentDraws.push_back(dc); break;
                        default:                                m_OpaqueDraws.push_back(dc);       break;
                    }
                }

                auto DrawBatch = [&](const std::vector<DrawCommand>& draws, Material::RenderMode mode)
                {
                    if (draws.empty()) return;

                    Material::CullMode currentCull = Material::CullMode::Back;
                    bool currentSkinned = false;
                    auto* pipeline = m_GeoPipelineManager.GetOrCreate(
                        pbrUUID, mode, currentCull, polyMode, m_PBRVertSpv, m_PBRFragSpv);
                    if (!pipeline) return;

                    pipeline->Bind(cmd);

                    for (const auto& dc : draws)
                    {
                        // Rebind pipeline if cull mode or skinned state changed
                        if (dc.cullMode != currentCull || dc.isSkinned != currentSkinned)
                        {
                            currentCull = dc.cullMode;
                            currentSkinned = dc.isSkinned;

                            VKPipeline* newPipeline = nullptr;
                            if (currentSkinned)
                            {
                                newPipeline = m_GeoSkinnedPipelineManager.GetOrCreate(
                                    pbrUUID, mode, currentCull, polyMode, m_PBRSkinnedVertSpv, m_PBRFragSpv);
                            }
                            else
                            {
                                newPipeline = m_GeoPipelineManager.GetOrCreate(
                                    pbrUUID, mode, currentCull, polyMode, m_PBRVertSpv, m_PBRFragSpv);
                            }
                            if (!newPipeline) continue;
                            newPipeline->Bind(cmd);
                        }

                        auto mesh = dc.model->GetMesh(dc.meshIndex);
                        auto vb = std::static_pointer_cast<VKVertexBuffer>(mesh->GetVertexBuffer());
                        auto ib = std::static_pointer_cast<VKIndexBuffer>(mesh->GetIndexBuffer());
                        if (!vb || !ib) continue;

                        VkBuffer vbuf[] = { vb->GetVulkanBuffer() };
                        VkDeviceSize offsets[] = { 0 };
                        vkCmdBindVertexBuffers(cmd, 0, 1, vbuf, offsets);
                        vkCmdBindIndexBuffer(cmd, ib->GetVulkanBuffer(), 0, VK_INDEX_TYPE_UINT32);

                        // Indirect draw — GPU cull has set instanceCount=0 for culled objects.
                        // gl_BaseInstance = firstInstance = dc.gpuObjectIndex → shader reads objects[gl_BaseInstance]
                        VkDeviceSize indirectOffset = dc.gpuObjectIndex * sizeof(VkDrawIndexedIndirectCommand);
                        vkCmdDrawIndexedIndirect(cmd, m_IndirectBuffer, indirectOffset, 1,
                            sizeof(VkDrawIndexedIndirectCommand));

                        // Capture for frame debugger
                        if (m_FrameDebugger.state == DebuggerState::CaptureRequested)
                        {
                            std::string entName = "Entity";
                            if (dc.entityIndex < m_EntityLookup.size())
                            {
                                auto ent = m_EntityLookup[dc.entityIndex];
                                if (ent != entt::null && registry.valid(ent) && registry.any_of<Component::Tag>(ent))
                                    entName = registry.get<Component::Tag>(ent).Value;
                            }
                            u32 vkCull = (currentCull == Material::CullMode::Back) ? VK_CULL_MODE_BACK_BIT
                                       : (currentCull == Material::CullMode::Front) ? VK_CULL_MODE_FRONT_BIT
                                       : VK_CULL_MODE_NONE;
                            m_FrameDebugger.CaptureIndirectDraw("GeometryPass",
                                dc.model->GetName() + "[" + std::to_string(dc.meshIndex) + "]",
                                entName, dc.entityIndex, ib->GetCount(),
                                dc.gpuObjectIndex, indirectOffset,
                                { "pbr", static_cast<u32>(mode), vkCull, polyMode, currentSkinned, true, true,
                                  mode == Material::RenderMode::Transparent || mode == Material::RenderMode::Fade });
                        }
                    }
                };

                DrawBatch(m_OpaqueDraws,       Material::RenderMode::Opaque);
                DrawBatch(m_CutoutDraws,      Material::RenderMode::Cutout);
                DrawBatch(m_TransparentDraws, Material::RenderMode::Transparent);

                m_FrameDebugger.EndCapturePass();
            }
        );
        return output;
    }

}
