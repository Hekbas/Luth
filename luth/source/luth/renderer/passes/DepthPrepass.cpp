#include "luthpch.h"
#include "luth/scene/systems/RenderingSystem.h"
#include "luth/renderer/RenderPipeline.h"
#include "luth/core/diagnostics/Profiler.h"
#include "luth/core/RenderSnapshot.h"
#include "luth/scene/Scene.h"
#include "luth/scene/Components.h"
#include "luth/renderer/Renderer.h"
#include "luth/renderer/material/MaterialSystem.h"
#include "luth/renderer/resources/BoneMatrixBuffer.h"
#include "luth/renderer/backend/vulkan/VulkanBackend.h"
#include "luth/renderer/backend/vulkan/VulkanContext.h"
#include "luth/renderer/backend/vulkan/VulkanTexture.h"
#include "luth/renderer/backend/vulkan/VulkanBuffer.h"
#include "luth/renderer/material/Material.h"
#include "luth/renderer/resources/Model.h"
#include "luth/resources/AssetManager.h"
#include <vma/vk_mem_alloc.h>

namespace Luth
{
    using namespace Component;

    // Camera-space Z-prepass. Fills SceneDepth with opaque geometry before
    // GTAO and the forward GeometryPass run. Opaque-only — cutouts and
    // transparents write their depth in GeometryPass (with LESS_EQUAL, so
    // opaque depth written here wins). Reuses indirect region 0 (the camera
    // frustum cull region), same as GeometryPass's opaque draws.
    RG::ResourceHandle RenderPipeline::AddDepthPrepass(
        RG::RenderGraph& rg,
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
                depthDesc.width  = m_CurrentView->targets->GetSceneDepth()->GetWidth();
                depthDesc.height = m_CurrentView->targets->GetSceneDepth()->GetHeight();
                depthDesc.format = RG::TextureFormat::D32_Float;

                auto vkDepth = std::static_pointer_cast<VKTexture>(m_CurrentView->targets->GetSceneDepth());
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
            [this](DepthPrepassData& data, RG::RenderPassContext& ctx)
            {
                VkCommandBuffer cmd = ctx.commandBuffer;

                m_System.GetFrameDebugger().BeginCapturePass(ctx.passIndex, "DepthPrepass", "SceneDepth", true,
                    { "depthPrepass", 0, VK_CULL_MODE_BACK_BIT, VK_POLYGON_MODE_FILL, false, true, true, false });

                if (!m_DepthPrepassPipeline) { LH_CORE_ERROR("DepthPrepass pipeline is null!"); m_System.GetFrameDebugger().EndCapturePass(); return; }

                VkDescriptorSet bindlessSet = VulkanContext::Get().GetBindlessSet().GetSet();
                VkDescriptorSet sets[] = {
                    m_CurrentViewResources->globalDescriptorSet,
                    bindlessSet,
                    MaterialSystem::GetDescriptorSet(),
                    m_Lighting.GetLightDescSet(),
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

                // Opaque-only: cutouts and transparents write their depth in
                // GeometryPass (LESS_EQUAL, so opaque depth written here wins).
                for (const auto& dc : m_System.GetDrawList().opaque)
                {
                    auto mesh = dc.model->GetMesh(dc.meshIndex);
                    auto vb = std::static_pointer_cast<VKVertexBuffer>(mesh->GetVertexBuffer());
                    auto ib = std::static_pointer_cast<VKIndexBuffer>(mesh->GetIndexBuffer());
                    if (!vb || !ib) continue;

                    if (dc.isSkinned != currentSkinned)
                    {
                        currentSkinned = dc.isSkinned;
                        if (currentSkinned && m_DepthPrepassSkinnedPipeline)
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

                    // Camera region (offset 0) within this view's range. Region indexing
                    // is 0-based within m_IndirectRegion; the heap-region's byte offset
                    // is added below to land in the live frame's allocator-returned region.
                    const u32 viewBaseRegion = m_CurrentView->viewIndex * RenderPipeline::k_IndirectRegionsPerView;
                    const u32 cmdIndex = viewBaseRegion * RenderPipeline::k_IndirectRegionStride + dc.gpuObjectIndex;
                    VkDeviceSize indirectOffset = m_IndirectRegion.offset + cmdIndex * sizeof(VkDrawIndexedIndirectCommand);
                    vkCmdDrawIndexedIndirect(cmd, m_IndirectRegion.buffer, indirectOffset, 1,
                        sizeof(VkDrawIndexedIndirectCommand));

                    if (m_System.GetFrameDebugger().state == DebuggerState::CaptureRequested)
                    {
                        std::string entName = "Entity";
                        const auto& tags = m_System.GetActiveSnapshot().tagsByEntity;
                        u32 idx = entt::to_entity(dc.entity);
                        if (idx < tags.size() && tags[idx])
                            entName = tags[idx];
                        m_System.GetFrameDebugger().CaptureIndirectDraw("DepthPrepass",
                            dc.model->GetName() + "[" + std::to_string(dc.meshIndex) + "]",
                            entName, dc.entityIndex, ib->GetCount(), dc.gpuObjectIndex, indirectOffset,
                            { "depthPrepass", 0, static_cast<u32>(VK_CULL_MODE_BACK_BIT),
                              VK_POLYGON_MODE_FILL, dc.isSkinned, true, true, false });
                    }
                }

                m_System.GetFrameDebugger().EndCapturePass();
            }
        );

        return depthHandle;
    }
}
