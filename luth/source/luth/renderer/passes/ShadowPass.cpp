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

    RG::ResourceHandle RenderingSystem::AddShadowPass(
        RG::RenderGraph& rg, entt::registry& registry, RG::BufferHandle indirectBufferHandle)
    {
        struct ShadowPassData {
            RG::ResourceHandle shadowTex;
            RG::BufferHandle   indirectBuf;
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

                // Declare indirect buffer read (triggers compute-write → indirect-read barrier)
                data.indirectBuf = builder.ReadIndirectBuffer(indirectBufferHandle);

                shadowHandle = data.shadowTex;
            },
            [this, &registry](ShadowPassData& data, RG::RenderPassContext& ctx)
            {
                VkCommandBuffer cmd = ctx.commandBuffer;

                m_FrameDebugger.BeginCapturePass("ShadowPass", "ShadowMap", true,
                    { "shadowDepth", 0, VK_CULL_MODE_FRONT_BIT, VK_POLYGON_MODE_FILL, false, true, true, false });

                if (!m_ShadowPipeline) { LH_CORE_ERROR("Shadow pipeline is null!"); m_FrameDebugger.EndCapturePass(); return; }

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

                // Start with static pipeline bound
                m_ShadowPipeline->Bind(cmd);
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    m_ShadowPipeline->GetLayout(), 0, 6, sets, 0, nullptr);

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

                    // Look up SSBO index for this entity
                    auto it = m_EntityToSSBOIndex.find(entity);
                    if (it == m_EntityToSSBOIndex.end()) continue;
                    u32 gpuObjectIndex = it->second;

                    // Per-mesh skinning flag
                    bool isSkinned = false;
                    if (meshRenderer.MeshIndex < model->GetMeshesData().size())
                        isSkinned = model->GetMeshesData()[meshRenderer.MeshIndex].IsSkinned;

                    // Switch pipeline if skinned state changed
                    if (isSkinned != currentSkinned)
                    {
                        currentSkinned = isSkinned;
                        if (isSkinned && m_ShadowSkinnedPipeline)
                        {
                            m_ShadowSkinnedPipeline->Bind(cmd);
                            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                m_ShadowSkinnedPipeline->GetLayout(), 0, 6, sets, 0, nullptr);
                        }
                        else
                        {
                            m_ShadowPipeline->Bind(cmd);
                            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                m_ShadowPipeline->GetLayout(), 0, 6, sets, 0, nullptr);
                        }
                    }

                    VkBuffer vbuf[] = { vb->GetVulkanBuffer() };
                    VkDeviceSize offsets[] = { 0 };
                    vkCmdBindVertexBuffers(cmd, 0, 1, vbuf, offsets);
                    vkCmdBindIndexBuffer(cmd, ib->GetVulkanBuffer(), 0, VK_INDEX_TYPE_UINT32);

                    // Indirect draw — GPU cull (main camera) has set instanceCount=0 for culled objects.
                    // gl_BaseInstance = firstInstance = gpuObjectIndex → shader reads objects[gl_BaseInstance]
                    VkDeviceSize indirectOffset = gpuObjectIndex * sizeof(VkDrawIndexedIndirectCommand);
                    vkCmdDrawIndexedIndirect(cmd, m_IndirectBuffer, indirectOffset, 1,
                        sizeof(VkDrawIndexedIndirectCommand));

                    // Capture for frame debugger
                    if (m_FrameDebugger.state == DebuggerState::CaptureRequested)
                    {
                        std::string entName = registry.any_of<Component::Tag>(entity)
                            ? registry.get<Component::Tag>(entity).m_Tag : "Entity";
                        u32 entityIndex = gpuObjectIndex + 1;
                        m_FrameDebugger.CaptureIndirectDraw("ShadowPass",
                            model->GetName() + "[" + std::to_string(meshRenderer.MeshIndex) + "]",
                            entName, entityIndex, ib->GetCount(), gpuObjectIndex, indirectOffset,
                            { "shadowDepth", 0, static_cast<u32>(VK_CULL_MODE_FRONT_BIT),
                              VK_POLYGON_MODE_FILL, isSkinned, true, true, false });
                    }
                }

                m_FrameDebugger.EndCapturePass();
            }
        );

        return shadowHandle;
    }

}
