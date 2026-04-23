#include "luthpch.h"
#include "luth/scene/systems/RenderingSystem.h"
#include "luth/renderer/RenderPipeline.h"
#include "luth/core/diagnostics/Profiler.h"
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

    void RenderPipeline::CollectSelectedHandles(const std::vector<Entity>& selected, std::unordered_set<entt::entity>& outHandles) const
    {
        for (const auto& entity : selected)
        {
            if (!entity || !entity.IsValid()) continue;
            outHandles.insert((entt::entity)entity);
            // Recursively include children so outline wraps entire subtrees
            for (const auto& child : entity.GetChildren())
            {
                std::vector<Entity> childVec = { child };
                CollectSelectedHandles(childVec, outHandles);
            }
        }
    }

    SelectionMaskOutput RenderPipeline::AddSelectionMaskPass(RG::RenderGraph& rg, entt::registry& registry)
    {
        struct SelectionMaskPassData {
            RG::ResourceHandle maskTex;
            RG::ResourceHandle depthTex;
        };

        SelectionMaskOutput output;

        rg.AddPass<SelectionMaskPassData>("SelectionMaskPass",
            [&](SelectionMaskPassData& data, RG::RenderPassBuilder& builder)
            {
                // Import selection mask (RGBA8)
                auto vkMask = std::static_pointer_cast<VKTexture>(m_CurrentView->targets->GetSelectionMask());
                RG::TextureDesc maskDesc;
                maskDesc.name   = "SelectionMask";
                maskDesc.width  = m_CurrentView->targets->GetSelectionMask()->GetWidth();
                maskDesc.height = m_CurrentView->targets->GetSelectionMask()->GetHeight();
                maskDesc.format = RG::TextureFormat::RGBA8_Unorm;

                data.maskTex = rg.ImportResource(maskDesc,
                    (void*)vkMask->GetImage(), (void*)vkMask->GetImageView(),
                    RG::ResourceState::Undefined);

                VkClearValue colorClear{};
                colorClear.color = {{0.0f, 0.0f, 0.0f, 0.0f}};
                data.maskTex = builder.Write(data.maskTex,
                    VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, colorClear);

                // Import selection depth (D32_Float)
                auto vkDepth = std::static_pointer_cast<VKTexture>(m_CurrentView->targets->GetSelectionDepth());
                RG::TextureDesc depthDesc;
                depthDesc.name   = "SelectionDepth";
                depthDesc.width  = m_CurrentView->targets->GetSelectionDepth()->GetWidth();
                depthDesc.height = m_CurrentView->targets->GetSelectionDepth()->GetHeight();
                depthDesc.format = RG::TextureFormat::D32_Float;

                data.depthTex = rg.ImportResource(depthDesc,
                    (void*)vkDepth->GetImage(), (void*)vkDepth->GetImageView(),
                    RG::ResourceState::Undefined);

                VkClearValue depthClear{};
                depthClear.depthStencil = { 1.0f, 0 };
                data.depthTex = builder.WriteDepth(data.depthTex,
                    VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, depthClear);

                output.mask  = data.maskTex;
                output.depth = data.depthTex;
            },
            [this, &registry](SelectionMaskPassData& data, RG::RenderPassContext& ctx)
            {
                m_System.m_FrameDebugger.BeginCapturePass("SelectionMaskPass", "SelectionMask", false,
                    { "selectionMask", 0, VK_CULL_MODE_BACK_BIT, VK_POLYGON_MODE_FILL, false, true, true, false });

                if (!m_SelectionMaskPipeline) { m_System.m_FrameDebugger.EndCapturePass(); return; }

                // Build set of selected entity handles (including descendants)
                std::unordered_set<entt::entity> selectedSet;
                CollectSelectedHandles(m_CurrentView->camera.selectedEntities, selectedSet);
                if (selectedSet.empty()) return;

                VkCommandBuffer cmd = ctx.commandBuffer;

                // Bind descriptor sets (same 5 sets as geometry/shadow passes)
                VkDescriptorSet bindlessSet = VulkanContext::Get().GetBindlessSet().GetSet();
                VkDescriptorSet sets[] = {
                    m_CurrentViewResources->globalDescriptorSet,
                    bindlessSet,
                    MaterialSystem::GetDescriptorSet(),
                    m_LightDescSet,
                    BoneMatrixBuffer::GetDescriptorSet()
                };

                m_SelectionMaskPipeline->Bind(cmd);
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    m_SelectionMaskPipeline->GetLayout(), 0, 5, sets, 0, nullptr);

                u32 w = m_CurrentView->targets->GetSelectionMask()->GetWidth();
                u32 h = m_CurrentView->targets->GetSelectionMask()->GetHeight();
                VkViewport vp{}; vp.width = (float)w; vp.height = (float)h; vp.maxDepth = 1.0f;
                vkCmdSetViewport(cmd, 0, 1, &vp);
                VkRect2D sc{}; sc.extent = { w, h };
                vkCmdSetScissor(cmd, 0, 1, &sc);

                bool currentSkinned = false;

                auto DrawBatch = [&](const std::vector<DrawCommand>& draws)
                {
                    for (const auto& dc : draws)
                    {
                        if (selectedSet.find(dc.entity) == selectedSet.end()) continue;

                        auto mesh = dc.model->GetMesh(dc.meshIndex);
                        auto vb = std::static_pointer_cast<VKVertexBuffer>(mesh->GetVertexBuffer());
                        auto ib = std::static_pointer_cast<VKIndexBuffer>(mesh->GetIndexBuffer());
                        if (!vb || !ib) continue;

                        if (dc.isSkinned != currentSkinned)
                        {
                            currentSkinned = dc.isSkinned;
                            if (currentSkinned && m_SelectionMaskSkinnedPipeline)
                            {
                                m_SelectionMaskSkinnedPipeline->Bind(cmd);
                                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    m_SelectionMaskSkinnedPipeline->GetLayout(), 0, 5, sets, 0, nullptr);
                            }
                            else
                            {
                                m_SelectionMaskPipeline->Bind(cmd);
                                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    m_SelectionMaskPipeline->GetLayout(), 0, 5, sets, 0, nullptr);
                            }
                        }

                        VkPipelineLayout activeLayout = (currentSkinned && m_SelectionMaskSkinnedPipeline)
                            ? m_SelectionMaskSkinnedPipeline->GetLayout()
                            : m_SelectionMaskPipeline->GetLayout();

                        ObjectPushConstants pc{};
                        pc.modelMatrix   = dc.modelMatrix;
                        pc.materialIndex = 0;
                        pc.boneOffset    = dc.boneOffset;

                        vkCmdPushConstants(cmd, activeLayout,
                            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                            0, sizeof(ObjectPushConstants), &pc);

                        VkBuffer vbuf[] = { vb->GetVulkanBuffer() };
                        VkDeviceSize offsets[] = { 0 };
                        vkCmdBindVertexBuffers(cmd, 0, 1, vbuf, offsets);
                        vkCmdBindIndexBuffer(cmd, ib->GetVulkanBuffer(), 0, VK_INDEX_TYPE_UINT32);
                        vkCmdDrawIndexed(cmd, ib->GetCount(), 1, 0, 0, 0);
                    }
                };

                DrawBatch(m_System.m_DrawList.opaque);
                DrawBatch(m_System.m_DrawList.cutout);
                DrawBatch(m_System.m_DrawList.transparent);

                m_System.m_FrameDebugger.EndCapturePass();
            }
        );

        return output;
    }

}
