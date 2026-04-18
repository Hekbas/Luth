#include "luthpch.h"
#include "luth/scene/systems/RenderingSystem.h"
#include "luth/core/Profiler.h"
#include "luth/scene/Scene.h"
#include "luth/scene/Components.h"
#include "luth/renderer/Renderer.h"
#include "luth/renderer/material/MaterialSystem.h"
#include "luth/renderer/BoneMatrixBuffer.h"
#include "luth/renderer/backend/vulkan/VulkanBackend.h"
#include "luth/renderer/backend/vulkan/VulkanContext.h"
#include "luth/renderer/backend/vulkan/VulkanTexture.h"
#include "luth/renderer/backend/vulkan/VulkanBuffer.h"
#include "luth/renderer/material/Material.h"
#include "luth/renderer/resources/Model.h"
#include "luth/resources/AssetManager.h"
#include <glm/gtc/matrix_transform.hpp>
#include <vma/vk_mem_alloc.h>

namespace Luth
{
    using namespace Component;

    // Camera-space Z-prepass. Fills SceneDepth with opaque geometry before
    // GTAO and the forward GeometryPass run. Opaque-only — cutouts and
    // transparents write their depth in GeometryPass (with LESS_EQUAL, so
    // opaque depth written here wins). Reuses indirect region 0 (the camera
    // frustum cull region), same as GeometryPass's opaque draws.
    RG::ResourceHandle RenderingSystem::AddDepthPrepass(
        RG::RenderGraph& rg, entt::registry& registry,
        RG::BufferHandle indirectBufferHandle)
    {
        struct DepthPrepassData {
            RG::ResourceHandle depthTex;
            RG::BufferHandle   indirectBuf;
        };

        RG::ResourceHandle depthHandle;

        rg.AddPass<DepthPrepassData>("DepthPrepass",
            [&](DepthPrepassData& data, RG::RenderPassBuilder& builder)
            {
                RG::TextureDesc depthDesc;
                depthDesc.name   = "SceneDepth";
                depthDesc.width  = m_Targets.GetSceneDepth()->GetWidth();
                depthDesc.height = m_Targets.GetSceneDepth()->GetHeight();
                depthDesc.format = RG::TextureFormat::D32_Float;

                auto vkDepth = std::static_pointer_cast<VKTexture>(m_Targets.GetSceneDepth());
                data.depthTex = rg.ImportResource(depthDesc,
                    (void*)vkDepth->GetImage(),
                    (void*)vkDepth->GetImageView(),
                    RG::ResourceState::Undefined);

                VkClearValue depthClear{};
                depthClear.depthStencil = { 1.0f, 0 };
                data.depthTex = builder.WriteDepth(data.depthTex,
                    VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, depthClear);

                data.indirectBuf = builder.ReadIndirectBuffer(indirectBufferHandle);

                depthHandle = data.depthTex;
            },
            [this, &registry](DepthPrepassData& data, RG::RenderPassContext& ctx)
            {
                VkCommandBuffer cmd = ctx.commandBuffer;

                m_FrameDebugger.BeginCapturePass("DepthPrepass", "SceneDepth", true,
                    { "depthPrepass", 0, VK_CULL_MODE_BACK_BIT, VK_POLYGON_MODE_FILL, false, true, true, false });

                if (!m_DepthPrepassPipeline) { LH_CORE_ERROR("DepthPrepass pipeline is null!"); m_FrameDebugger.EndCapturePass(); return; }

                VkDescriptorSet bindlessSet = VulkanContext::Get().GetBindlessSet().GetSet();
                VkDescriptorSet sets[] = {
                    m_GlobalDescriptorSet,
                    bindlessSet,
                    MaterialSystem::GetDescriptorSet(),
                    m_LightDescSet,
                    BoneMatrixBuffer::GetDescriptorSet(),
                    m_ObjectSSBODescSet
                };

                m_DepthPrepassPipeline->Bind(cmd);
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    m_DepthPrepassPipeline->GetLayout(), 0, 6, sets, 0, nullptr);

                RG::RenderGraph::ResourceNode* res = (RG::RenderGraph::ResourceNode*)ctx.GetResource(data.depthTex);
                VkViewport viewport{};
                viewport.width    = (float)res->desc.width;
                viewport.height   = (float)res->desc.height;
                viewport.maxDepth = 1.0f;
                vkCmdSetViewport(cmd, 0, 1, &viewport);

                VkRect2D scissor{};
                scissor.extent = { res->desc.width, res->desc.height };
                vkCmdSetScissor(cmd, 0, 1, &scissor);

                bool currentSkinned = false;

                auto view = registry.view<WorldTransform, MeshRenderer>();
                for (auto [entity, worldTransform, meshRenderer] : view.each())
                {
                    auto model = AssetManager::GetAsset<Model>(meshRenderer.ModelUUID);
                    if (!model) continue;
                    auto mesh = model->GetMesh(meshRenderer.MeshIndex);
                    if (!mesh) continue;

                    // Opaque-only: skip cutout/transparent. They shade in GeometryPass.
                    Material::RenderMode mode = Material::RenderMode::Opaque;
                    if (meshRenderer.MaterialUUID.IsValid())
                    {
                        auto material = AssetManager::GetAsset<Material>(meshRenderer.MaterialUUID);
                        if (material) mode = material->GetRenderMode();
                    }
                    if (mode != Material::RenderMode::Opaque) continue;

                    auto vb = std::static_pointer_cast<VKVertexBuffer>(mesh->GetVertexBuffer());
                    auto ib = std::static_pointer_cast<VKIndexBuffer>(mesh->GetIndexBuffer());
                    if (!vb || !ib) continue;

                    auto it = m_EntityToSSBOIndex.find(entity);
                    if (it == m_EntityToSSBOIndex.end()) continue;
                    u32 gpuObjectIndex = it->second;

                    bool isSkinned = false;
                    if (meshRenderer.MeshIndex < model->GetMeshesData().size())
                        isSkinned = model->GetMeshesData()[meshRenderer.MeshIndex].IsSkinned;

                    if (isSkinned != currentSkinned)
                    {
                        currentSkinned = isSkinned;
                        if (isSkinned && m_DepthPrepassSkinnedPipeline)
                        {
                            m_DepthPrepassSkinnedPipeline->Bind(cmd);
                            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                m_DepthPrepassSkinnedPipeline->GetLayout(), 0, 6, sets, 0, nullptr);
                        }
                        else
                        {
                            m_DepthPrepassPipeline->Bind(cmd);
                            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                m_DepthPrepassPipeline->GetLayout(), 0, 6, sets, 0, nullptr);
                        }
                    }

                    VkBuffer vbuf[] = { vb->GetVulkanBuffer() };
                    VkDeviceSize offsets[] = { 0 };
                    vkCmdBindVertexBuffers(cmd, 0, 1, vbuf, offsets);
                    vkCmdBindIndexBuffer(cmd, ib->GetVulkanBuffer(), 0, VK_INDEX_TYPE_UINT32);

                    // Camera region of the indirect buffer (region 0).
                    VkDeviceSize indirectOffset = gpuObjectIndex * sizeof(VkDrawIndexedIndirectCommand);
                    vkCmdDrawIndexedIndirect(cmd, m_IndirectBuffer, indirectOffset, 1,
                        sizeof(VkDrawIndexedIndirectCommand));

                    if (m_FrameDebugger.state == DebuggerState::CaptureRequested)
                    {
                        std::string entName = registry.any_of<Component::Tag>(entity)
                            ? registry.get<Component::Tag>(entity).Value : "Entity";
                        u32 entityIndex = gpuObjectIndex + 1;
                        m_FrameDebugger.CaptureIndirectDraw("DepthPrepass",
                            model->GetName() + "[" + std::to_string(meshRenderer.MeshIndex) + "]",
                            entName, entityIndex, ib->GetCount(), gpuObjectIndex, indirectOffset,
                            { "depthPrepass", 0, static_cast<u32>(VK_CULL_MODE_BACK_BIT),
                              VK_POLYGON_MODE_FILL, isSkinned, true, true, false });
                    }
                }

                m_FrameDebugger.EndCapturePass();
            }
        );

        return depthHandle;
    }
}
