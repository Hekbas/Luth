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
#include <glm/gtc/matrix_transform.hpp>
#include <vma/vk_mem_alloc.h>

namespace Luth
{
    using namespace Component;

    // =========================================================================
    // Render Graph Passes
    // =========================================================================

    RG::ResourceHandle RenderingSystem::AddShadowPass(RG::RenderGraph& rg, entt::registry& registry)
    {
        struct ShadowPassData {
            RG::ResourceHandle shadowTex;
        };

        RG::ResourceHandle shadowHandle;

        rg.AddPass<ShadowPassData>("ShadowPass",
            [&](ShadowPassData& data, RG::RenderPassBuilder& builder)
            {
                auto vkShadowTex = std::static_pointer_cast<VKTexture>(m_ShadowMap);

                RG::TextureDesc desc;
                desc.name   = "ShadowMap";
                desc.width  = 2048;
                desc.height = 2048;
                desc.format = RG::TextureFormat::D32_Float;

                data.shadowTex = rg.ImportResource(desc,
                    (void*)vkShadowTex->GetImage(),
                    (void*)vkShadowTex->GetImageView(),
                    RG::ResourceState::Undefined);

                VkClearValue depthClear{};
                depthClear.depthStencil = { 1.0f, 0 };
                // STORE so the depth values are kept for the geometry pass to read
                data.shadowTex = builder.WriteDepth(data.shadowTex,
                    VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, depthClear);

                shadowHandle = data.shadowTex;
            },
            [this, &registry](ShadowPassData& data, RG::RenderPassContext& ctx)
            {
                VkCommandBuffer cmd = ctx.commandBuffer;

                m_FrameDebugger.BeginCapturePass("ShadowPass", "ShadowMap", true,
                    { "shadowDepth", 0, VK_CULL_MODE_FRONT_BIT, VK_POLYGON_MODE_FILL, false, true, true, false });

                if (!m_ShadowPipeline) { LH_CORE_ERROR("Shadow pipeline is null!"); m_FrameDebugger.EndCapturePass(); return; }

                // Bind all 5 descriptor sets
                VkDescriptorSet bindlessSet = VulkanContext::Get().GetBindlessSet().GetSet();
                VkDescriptorSet sets[] = {
                    m_GlobalDescriptorSet,
                    bindlessSet,
                    MaterialSystem::GetDescriptorSet(),
                    m_LightDescSet,
                    BoneMatrixBuffer::GetDescriptorSet()
                };

                // Start with static pipeline bound
                m_ShadowPipeline->Bind(cmd);
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    m_ShadowPipeline->GetLayout(), 0, 5, sets, 0, nullptr);

                // Shadow map viewport
                VkViewport viewport{};
                viewport.width    = 2048.0f;
                viewport.height   = 2048.0f;
                viewport.maxDepth = 1.0f;
                vkCmdSetViewport(cmd, 0, 1, &viewport);

                VkRect2D scissor{};
                scissor.extent = { 2048, 2048 };
                vkCmdSetScissor(cmd, 0, 1, &scissor);

                bool currentSkinned = false;

                auto view = registry.view<WorldTransform, MeshRenderer>();
                for (auto [entity, worldTransform, meshRenderer] : view.each())
                {
                    auto model = AssetManager::GetAsset<Model>(meshRenderer.ModelUUID);
                    if (!model) continue;
                    auto mesh = model->GetMesh(meshRenderer.MeshIndex);
                    if (!mesh) continue;

                    auto vb = std::static_pointer_cast<VKVertexBuffer>(mesh->GetVertexBuffer());
                    auto ib = std::static_pointer_cast<VKIndexBuffer>(mesh->GetIndexBuffer());
                    if (!vb || !ib) continue;

                    // Check per-mesh skinning
                    bool isSkinned = false;
                    u32 boneOffset = 0;
                    if (meshRenderer.MeshIndex < model->GetMeshesData().size())
                        isSkinned = model->GetMeshesData()[meshRenderer.MeshIndex].IsSkinned;

                    if (isSkinned)
                    {
                        // Find Animation on this entity or parent
                        entt::entity animEntity = entt::null;
                        if (registry.any_of<Component::Animation>(entity))
                            animEntity = entity;
                        else if (registry.any_of<Component::Parent>(entity)) {
                            auto parentEnt = (entt::entity)registry.get<Component::Parent>(entity).m_Parent;
                            if (registry.valid(parentEnt) && registry.any_of<Component::Animation>(parentEnt))
                                animEntity = parentEnt;
                        }
                        if (animEntity != entt::null) {
                            auto& anim = registry.get<Component::Animation>(animEntity);
                            if (anim.BufferAllocated)
                                boneOffset = anim.BoneBufferOffset;
                        }
                    }

                    // Switch pipeline if skinned state changed
                    if (isSkinned != currentSkinned)
                    {
                        currentSkinned = isSkinned;
                        if (isSkinned && m_ShadowSkinnedPipeline)
                        {
                            m_ShadowSkinnedPipeline->Bind(cmd);
                            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                m_ShadowSkinnedPipeline->GetLayout(), 0, 5, sets, 0, nullptr);
                        }
                        else
                        {
                            m_ShadowPipeline->Bind(cmd);
                            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                m_ShadowPipeline->GetLayout(), 0, 5, sets, 0, nullptr);
                        }
                    }

                    VkPipelineLayout activeLayout = (currentSkinned && m_ShadowSkinnedPipeline)
                        ? m_ShadowSkinnedPipeline->GetLayout()
                        : m_ShadowPipeline->GetLayout();

                    ObjectPushConstants pc{};
                    pc.modelMatrix   = worldTransform.Matrix;
                    pc.materialIndex = 0;
                    pc.boneOffset    = boneOffset;

                    vkCmdPushConstants(cmd, activeLayout,
                        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                        0, sizeof(ObjectPushConstants), &pc);

                    VkBuffer vbuf[] = { vb->GetVulkanBuffer() };
                    VkDeviceSize offsets[] = { 0 };
                    vkCmdBindVertexBuffers(cmd, 0, 1, vbuf, offsets);
                    vkCmdBindIndexBuffer(cmd, ib->GetVulkanBuffer(), 0, VK_INDEX_TYPE_UINT32);
                    vkCmdDrawIndexed(cmd, ib->GetCount(), 1, 0, 0, 0);

                    // Capture draw call for frame debugger
                    m_FrameDebugger.CaptureDrawCall("ShadowPass",
                        model->GetName() + "[" + std::to_string(meshRenderer.MeshIndex) + "]",
                        registry.any_of<Component::Tag>(entity) ? registry.get<Component::Tag>(entity).m_Tag : "Entity",
                        0, ib->GetCount(), pc,
                        { "shadowDepth", 0, static_cast<u32>(VK_CULL_MODE_FRONT_BIT),
                          VK_POLYGON_MODE_FILL, isSkinned, true, true, false });
                }

                m_FrameDebugger.EndCapturePass();
            }
        );

        return shadowHandle;
    }

}
