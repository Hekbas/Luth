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

    // =========================================================================
    // Render Graph Passes
    // =========================================================================

    RG::ResourceHandle RenderPipeline::AddShadowPass(
        RG::RenderGraph& rg, RG::BufferHandle indirectBufferHandle, u32 cascadeIndex)
    {
        struct ShadowPassData {
            RG::ResourceHandle shadowTex;
            RG::BufferHandle   indirectBuf;
            u32                cascadeIndex;
        };

        RG::ResourceHandle shadowHandle;
        const std::string passName = "ShadowPass.C" + std::to_string(cascadeIndex);
        const std::string resName  = "ShadowMap.C" + std::to_string(cascadeIndex);

        rg.AddPass<ShadowPassData>(passName,
            [&](ShadowPassData& data, RG::RenderPassBuilder& builder)
            {
                data.cascadeIndex = cascadeIndex;

                auto vkShadowTex = std::static_pointer_cast<VKTexture>(m_ShadowMap);

                RG::TextureDesc desc;
                desc.name   = resName;
                desc.width  = k_ShadowResolution;
                desc.height = k_ShadowResolution;
                desc.format = RG::TextureFormat::D32_Float;

                // Import a per-layer view so the depth attachment targets cascade `i` only.
                // Barriers issued by the graph will carry baseArrayLayer=cascadeIndex, layerCount=1.
                data.shadowTex = rg.ImportResource(desc,
                    (void*)vkShadowTex->GetImage(),
                    (void*)m_ShadowLayerViews[cascadeIndex],
                    RG::ResourceState::Undefined,
                    /*baseArrayLayer*/ cascadeIndex,
                    /*layerCount*/     1);

                VkClearValue depthClear{};
                depthClear.depthStencil = { 1.0f, 0 };
                // STORE so the depth values are kept for the geometry pass to read
                data.shadowTex = builder.WriteDepth(data.shadowTex,
                    VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, depthClear);

                // Declare indirect buffer read (triggers compute-write → indirect-read barrier)
                data.indirectBuf = builder.ReadIndirectBuffer(indirectBufferHandle);

                shadowHandle = data.shadowTex;
            },
            [this, passName, resName](ShadowPassData& data, RG::RenderPassContext& ctx)
            {
                VkCommandBuffer cmd = ctx.commandBuffer;

                m_System.m_FrameDebugger.BeginCapturePass(ctx.passIndex, passName, resName, true,
                    { "shadowDepth", 0, VK_CULL_MODE_FRONT_BIT, VK_POLYGON_MODE_FILL, false, true, true, false });

                if (!m_ShadowPipeline) { LH_CORE_ERROR("Shadow pipeline is null!"); m_System.m_FrameDebugger.EndCapturePass(); return; }

                // Bind all 6 descriptor sets (Set 5 = GPUObjectData SSBO)
                VkDescriptorSet bindlessSet = VulkanContext::Get().GetBindlessSet().GetSet();
                VkDescriptorSet sets[] = {
                    m_CurrentViewResources->globalDescriptorSet,
                    bindlessSet,
                    MaterialSystem::GetDescriptorSet(),
                    m_LightDescSet,
                    BoneMatrixBuffer::GetDescriptorSet(),
                    m_ObjectSSBODescSet
                };

                // Start with static pipeline bound
                m_ShadowPipeline->Bind(cmd);
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    m_ShadowPipeline->GetLayout(), 0, 6, sets, 0, nullptr);

                // Push cascadeIndex so the vertex shader selects lightSpaceMatrix[pc.cascadeIndex].
                const u32 cascadeIdxVal = data.cascadeIndex;
                vkCmdPushConstants(cmd, m_ShadowPipeline->GetLayout(),
                    VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(u32), &cascadeIdxVal);

                // Shadow map viewport
                VkViewport viewport{};
                viewport.width    = (float)k_ShadowResolution;
                viewport.height   = (float)k_ShadowResolution;
                viewport.maxDepth = 1.0f;
                vkCmdSetViewport(cmd, 0, 1, &viewport);

                VkRect2D scissor{};
                scissor.extent = { k_ShadowResolution, k_ShadowResolution };
                vkCmdSetScissor(cmd, 0, 1, &scissor);

                bool currentSkinned = false;

                auto DrawBatch = [&](const std::vector<DrawCommand>& draws)
                {
                    for (const auto& dc : draws)
                    {
                        auto mesh = dc.model->GetMesh(dc.meshIndex);
                        auto vb = std::static_pointer_cast<VKVertexBuffer>(mesh->GetVertexBuffer());
                        auto ib = std::static_pointer_cast<VKIndexBuffer>(mesh->GetIndexBuffer());
                        if (!vb || !ib) continue;

                        // Switch pipeline if skinned state changed
                        if (dc.isSkinned != currentSkinned)
                        {
                            currentSkinned = dc.isSkinned;
                            if (currentSkinned && m_ShadowSkinnedPipeline)
                            {
                                m_ShadowSkinnedPipeline->Bind(cmd);
                                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    m_ShadowSkinnedPipeline->GetLayout(), 0, 6, sets, 0, nullptr);
                                vkCmdPushConstants(cmd, m_ShadowSkinnedPipeline->GetLayout(),
                                    VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(u32), &cascadeIdxVal);
                            }
                            else
                            {
                                m_ShadowPipeline->Bind(cmd);
                                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    m_ShadowPipeline->GetLayout(), 0, 6, sets, 0, nullptr);
                                vkCmdPushConstants(cmd, m_ShadowPipeline->GetLayout(),
                                    VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(u32), &cascadeIdxVal);
                            }
                        }

                        VkBuffer vbuf[] = { vb->GetVulkanBuffer() };
                        VkDeviceSize offsets[] = { 0 };
                        vkCmdBindVertexBuffers(cmd, 0, 1, vbuf, offsets);
                        vkCmdBindIndexBuffer(cmd, ib->GetVulkanBuffer(), 0, VK_INDEX_TYPE_UINT32);

                        // Per-view region layout: [camera | C0 | C1 | C2 | C3].
                        // View N starts at region (N * k_IndirectRegionsPerView).
                        const u32 viewBaseRegion = m_CurrentView->viewIndex * RenderPipeline::k_IndirectRegionsPerView;
                        const u32 cmdIndex = (viewBaseRegion + data.cascadeIndex + 1) * RenderPipeline::k_IndirectRegionStride + dc.gpuObjectIndex;
                        VkDeviceSize indirectOffset = cmdIndex * sizeof(VkDrawIndexedIndirectCommand);
                        vkCmdDrawIndexedIndirect(cmd, m_IndirectBuffer, indirectOffset, 1,
                            sizeof(VkDrawIndexedIndirectCommand));

                        if (m_System.m_FrameDebugger.state == DebuggerState::CaptureRequested)
                        {
                            std::string entName = "Entity";
                            const auto& tags = m_System.GetActiveSnapshot().tagsByEntity;
                            u32 idx = entt::to_entity(dc.entity);
                            if (idx < tags.size() && tags[idx])
                                entName = tags[idx];
                            m_System.m_FrameDebugger.CaptureIndirectDraw(passName,
                                dc.model->GetName() + "[" + std::to_string(dc.meshIndex) + "]",
                                entName, dc.entityIndex, ib->GetCount(), dc.gpuObjectIndex, indirectOffset,
                                { "shadowDepth", 0, static_cast<u32>(VK_CULL_MODE_FRONT_BIT),
                                  VK_POLYGON_MODE_FILL, dc.isSkinned, true, true, false });
                        }
                    }
                };

                // Shadow casters = all visible geometry (opaque + cutout + transparent).
                DrawBatch(m_System.m_DrawList.opaque);
                DrawBatch(m_System.m_DrawList.cutout);
                DrawBatch(m_System.m_DrawList.transparent);

                m_System.m_FrameDebugger.EndCapturePass();
            }
        );

        return shadowHandle;
    }

}
