#include "luthpch.h"
#include "luth/renderer/subsystems/TransparencySubsystem.h"

#include "luth/renderer/RenderPipeline.h"
#include "luth/renderer/backend/vulkan/VulkanContext.h"
#include "luth/renderer/backend/vulkan/VulkanTexture.h"
#include "luth/renderer/backend/vulkan/VulkanBuffer.h"
#include "luth/renderer/material/MaterialSystem.h"
#include "luth/renderer/resources/BoneMatrixBuffer.h"
#include "luth/renderer/resources/Model.h"
#include "luth/renderer/shader/ShaderLibrary.h"
#include "luth/renderer/Renderer.h"
#include "luth/scene/systems/RenderingSystem.h"
#include "luth/core/diagnostics/Log.h"

#include <algorithm>

namespace Luth
{
    namespace {
        BufferLayout MakePBRVertexLayout() {
            return BufferLayout{
                { ShaderDataType::Float3, "a_Position"  },
                { ShaderDataType::Float3, "a_Normal"    },
                { ShaderDataType::Float2, "a_TexCoord0" },
                { ShaderDataType::Float2, "a_TexCoord1" },
                { ShaderDataType::Float3, "a_Tangent"   }
            };
        }
        BufferLayout MakeSkinnedVertexLayout() {
            return BufferLayout{
                { ShaderDataType::Float3, "a_Position"    },
                { ShaderDataType::Float3, "a_Normal"      },
                { ShaderDataType::Float2, "a_TexCoord0"   },
                { ShaderDataType::Float2, "a_TexCoord1"   },
                { ShaderDataType::Float3, "a_Tangent"     },
                { ShaderDataType::Int4,   "a_BoneIDs"     },
                { ShaderDataType::Float4, "a_BoneWeights" }
            };
        }
    }

    void TransparencySubsystem::Init(RenderPipeline& pipeline)
    {
        m_Pipeline = &pipeline;
        VkDevice device = VulkanContext::Get().GetDevice();

        // Set 6 — b0 fog atlas (sampler3D, parity-rewritten per frame → UAB), b1 OIT heads storage
        // image + b2 OIT nodes SSBO (UAB + PARTIALLY_BOUND: unwritten until the PPLL passes land;
        // the sorted pipeline never statically uses them, which partially-bound makes legal).
        VkDescriptorSetLayoutBinding bindings[3]{};
        bindings[0].binding         = 0;
        bindings[0].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[0].descriptorCount = 1;
        bindings[0].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
        bindings[1].binding         = 1;
        bindings[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        bindings[1].descriptorCount = 1;
        bindings[1].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
        bindings[2].binding         = 2;
        bindings[2].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[2].descriptorCount = 1;
        bindings[2].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

        VkDescriptorBindingFlags bindingFlags[3] = {
            VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT,
            VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT | VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT,
            VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT | VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT,
        };
        VkDescriptorSetLayoutBindingFlagsCreateInfo bindingFlagsCI{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO };
        bindingFlagsCI.bindingCount  = 3;
        bindingFlagsCI.pBindingFlags = bindingFlags;

        VkDescriptorSetLayoutCreateInfo layoutCI{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        layoutCI.pNext        = &bindingFlagsCI;
        layoutCI.flags        = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
        layoutCI.bindingCount = 3;
        layoutCI.pBindings    = bindings;
        vkCreateDescriptorSetLayout(device, &layoutCI, nullptr, &m_TransparentSetLayout);

        // OIT resolve layout — b0 heads (storage image), b1 nodes (SSBO). Stable per-view; rewritten
        // only by WriteOitView on alloc/resize, so no UAB flags needed.
        VkDescriptorSetLayoutBinding resolveBindings[2]{};
        resolveBindings[0].binding         = 0;
        resolveBindings[0].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        resolveBindings[0].descriptorCount = 1;
        resolveBindings[0].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
        resolveBindings[1].binding         = 1;
        resolveBindings[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        resolveBindings[1].descriptorCount = 1;
        resolveBindings[1].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

        VkDescriptorSetLayoutCreateInfo resolveCI{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        resolveCI.bindingCount = 2;
        resolveCI.pBindings    = resolveBindings;
        vkCreateDescriptorSetLayout(device, &resolveCI, nullptr, &m_ResolveSetLayout);
    }

    void TransparencySubsystem::BuildPipelines(const std::vector<VkDescriptorSetLayout>& geoLayouts)
    {
        if (auto sh = ShaderLibrary::LoadEngine("shaders/pbr_transparent.frag"))
            m_TransparentFragSpv = sh->GetSpirV();
        if (m_TransparentFragSpv.empty())
        {
            LH_CORE_ERROR("TransparencySubsystem: failed to load pbr_transparent.frag!");
            return;
        }

        std::vector<VkDescriptorSetLayout> layouts = geoLayouts;
        layouts.push_back(m_TransparentSetLayout);

        const VkPushConstantRange pcRange{ VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(TransparentPC) };

        // Mirrors GeometrySubsystem's Transparent/Fade arm: same attachments as GeometryPass
        // (sceneColor + entityID + depth), depth-test-no-write LESS_OR_EQUAL, standard alpha blend.
        auto makeFactory = [pcRange](BufferLayout layout) {
            auto bindingDescs = layout.GetBindingDescriptions();
            auto attribDescs  = layout.GetAttributeDescriptions();
            return [bindingDescs, attribDescs, pcRange](Material::RenderMode, Material::CullMode cullMode,
                                                        VkPolygonMode polygonMode) -> PipelineConfig
            {
                PipelineConfig config;
                config.colorFormats          = { VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R32_UINT };
                config.depthFormat           = VK_FORMAT_D32_SFLOAT;
                config.frontFace             = VK_FRONT_FACE_COUNTER_CLOCKWISE;
                config.bindingDescriptions   = bindingDescs;
                config.attributeDescriptions = attribDescs;
                config.polygonMode           = polygonMode;
                config.depthCompareOp        = VK_COMPARE_OP_LESS_OR_EQUAL;
                config.depthTest             = true;
                config.depthWrite            = false;
                config.blendEnabled          = true;
                config.pushConstantRanges    = { pcRange };
                switch (cullMode)
                {
                    case Material::CullMode::Back:  config.cullMode = VK_CULL_MODE_BACK_BIT;  break;
                    case Material::CullMode::Front: config.cullMode = VK_CULL_MODE_FRONT_BIT; break;
                    case Material::CullMode::None:  config.cullMode = VK_CULL_MODE_NONE;      break;
                }
                return config;
            };
        };

        m_SortedPm.Init(layouts, makeFactory(MakePBRVertexLayout()));
        m_SortedSkinnedPm.Init(layouts, makeFactory(MakeSkinnedVertexLayout()));
    }

    void TransparencySubsystem::Shutdown()
    {
        m_SortedPm.Shutdown();
        m_SortedSkinnedPm.Shutdown();
        VkDevice device = VulkanContext::Get().GetDevice();
        if (m_TransparentSetLayout != VK_NULL_HANDLE)
        {
            vkDestroyDescriptorSetLayout(device, m_TransparentSetLayout, nullptr);
            m_TransparentSetLayout = VK_NULL_HANDLE;
        }
        if (m_ResolveSetLayout != VK_NULL_HANDLE)
        {
            vkDestroyDescriptorSetLayout(device, m_ResolveSetLayout, nullptr);
            m_ResolveSetLayout = VK_NULL_HANDLE;
        }
    }

    bool TransparencySubsystem::OnShaderReloaded(const std::string& name, const std::vector<u32>& spv)
    {
        if (name == "pbr_transparent.frag")
        {
            m_TransparentFragSpv = spv;
            if (auto sh = ShaderLibrary::Get("pbr_transparent.frag"))
            {
                m_SortedPm.DeferredInvalidateShader(sh->Handle);
                m_SortedSkinnedPm.DeferredInvalidateShader(sh->Handle);
            }
            return true;
        }
        // Vert reloads are owned by GeometrySubsystem (cached spv there); our variants compiled
        // against the old spv must still drop. Return false so the geometry handler runs too.
        if (name == "pbr.vert" || name == "pbr_skinned.vert")
        {
            if (auto sh = ShaderLibrary::Get("pbr_transparent.frag"))
            {
                m_SortedPm.DeferredInvalidateShader(sh->Handle);
                m_SortedSkinnedPm.DeferredInvalidateShader(sh->Handle);
            }
        }
        return false;
    }

    void TransparencySubsystem::WritePerFrame(ViewResources& vr, u32 frameAbs)
    {
        if (m_TransparentSetLayout == VK_NULL_HANDLE) return;
        if (!vr.volInScatterHistA || !vr.volInScatterHistB) return;

        const u32  slot   = frameAbs % MAX_FRAMES_IN_FLIGHT;
        const bool parity = (frameAbs & 1u) != 0u;
        if (vr.transparentDescSet[slot] == VK_NULL_HANDLE) return;

        // Same parity rule as the volumetric composite's b1 — sample this frame's resolved atlas.
        auto vkScat = std::static_pointer_cast<VKTexture>(
            parity ? vr.volInScatterHistA : vr.volInScatterHistB);

        VkDescriptorImageInfo scatInfo{};
        scatInfo.imageView   = vkScat->GetImageView();
        scatInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        scatInfo.sampler     = m_Pipeline->GetVolumetric().GetSampler();

        VkWriteDescriptorSet write{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        write.dstSet          = vr.transparentDescSet[slot];
        write.dstBinding      = 0;
        write.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.descriptorCount = 1;
        write.pImageInfo      = &scatInfo;
        vkUpdateDescriptorSets(VulkanContext::Get().GetDevice(), 1, &write, 0, nullptr);
    }

    void TransparencySubsystem::WriteOitView(ViewResources& vr)
    {
        if (m_TransparentSetLayout == VK_NULL_HANDLE) return;
        if (!vr.oitHeads || vr.oitNodes.buffer == VK_NULL_HANDLE) return;

        auto vkHeads = std::static_pointer_cast<VKTexture>(vr.oitHeads);

        VkDescriptorImageInfo headsInfo{};
        headsInfo.imageView   = vkHeads->GetImageView();
        headsInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        // Bind through the producer's GPUSubRegion {buffer, offset, size} — RG BufferHandles are
        // barrier bookkeeping only. see arch/rendering-pipeline.md (hazard 3)
        VkDescriptorBufferInfo nodesInfo{};
        nodesInfo.buffer = vr.oitNodes.buffer;
        nodesInfo.offset = vr.oitNodes.offset;
        nodesInfo.range  = vr.oitNodes.size;

        std::vector<VkWriteDescriptorSet> writes;
        writes.reserve(MAX_FRAMES_IN_FLIGHT * 2 + 2);
        for (u32 slot = 0; slot < MAX_FRAMES_IN_FLIGHT; ++slot)
        {
            if (vr.transparentDescSet[slot] == VK_NULL_HANDLE) continue;
            VkWriteDescriptorSet w{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            w.dstSet          = vr.transparentDescSet[slot];
            w.dstBinding      = 1;
            w.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            w.descriptorCount = 1;
            w.pImageInfo      = &headsInfo;
            writes.push_back(w);
            w.dstBinding      = 2;
            w.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            w.pImageInfo      = nullptr;
            w.pBufferInfo     = &nodesInfo;
            writes.push_back(w);
        }
        if (vr.oitResolveDescSet != VK_NULL_HANDLE)
        {
            VkWriteDescriptorSet w{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            w.dstSet          = vr.oitResolveDescSet;
            w.dstBinding      = 0;
            w.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            w.descriptorCount = 1;
            w.pImageInfo      = &headsInfo;
            writes.push_back(w);
            w.dstBinding      = 1;
            w.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            w.pImageInfo      = nullptr;
            w.pBufferInfo     = &nodesInfo;
            writes.push_back(w);
        }
        if (!writes.empty())
            vkUpdateDescriptorSets(VulkanContext::Get().GetDevice(),
                static_cast<u32>(writes.size()), writes.data(), 0, nullptr);
    }

    RG::ResourceHandle TransparencySubsystem::AddPasses(RG::RenderGraph& rg,
                                                        RG::ResourceHandle sceneColor,
                                                        RG::ResourceHandle entityID,
                                                        RG::ResourceHandle sceneDepth,
                                                        RG::ResourceHandle fogResolved,
                                                        RG::BufferHandle indirectBufferHandle)
    {
        auto& sys = m_Pipeline->GetSystem();
        if (sys.GetDrawList().transparent.empty())
            return sceneColor;
        return AddSortedPass(rg, sceneColor, entityID, sceneDepth, fogResolved, indirectBufferHandle);
    }

    RG::ResourceHandle TransparencySubsystem::AddSortedPass(RG::RenderGraph& rg,
                                                            RG::ResourceHandle sceneColor,
                                                            RG::ResourceHandle entityID,
                                                            RG::ResourceHandle sceneDepth,
                                                            RG::ResourceHandle fogResolved,
                                                            RG::BufferHandle indirectBufferHandle)
    {
        auto& sys = m_Pipeline->GetSystem();
        const auto& draws = sys.GetDrawList().transparent;
        const u32 n = static_cast<u32>(draws.size());

        // Per-view back-to-front order over the SHARED transparent bucket — an index array, never an
        // in-place sort (the other view + the OIT store iterate the same vector). Key = view-space
        // depth of the world-space bind-pose bounds center; scratch from the per-frame LinearAllocator
        // (reset at Update entry; the RG records within the same Update body).
        auto& alloc = sys.GetFrameAllocator();
        u32* order = static_cast<u32*>(alloc.Allocate(n * sizeof(u32), alignof(u32)));
        f32* keys  = static_cast<f32*>(alloc.Allocate(n * sizeof(f32), alignof(f32)));
        const Mat4& viewMat = m_Pipeline->GetCurrentView()->camera.view;
        for (u32 i = 0; i < n; ++i)
        {
            order[i] = i;
            const DrawCommand& dc = draws[i];
            Vec3 center(0.0f);
            if (dc.model && dc.meshIndex < dc.model->GetMeshesData().size())
                center = dc.model->GetMeshesData()[dc.meshIndex].BindPoseAABB.Center();
            const Vec4 worldCenter = dc.modelMatrix * Vec4(center, 1.0f);
            keys[i] = -(viewMat * worldCenter).z;   // RH view space: -z in front → key = distance
        }
        std::sort(order, order + n, [keys](u32 a, u32 b) { return keys[a] > keys[b]; });

        struct TransparentPassData
        {
            RG::ResourceHandle color, id, depth, fog;
            RG::BufferHandle   indirect;
            const u32*         order = nullptr;
            u32                count = 0;
            bool               fogValid = false;
        };
        RG::ResourceHandle outputHandle;

        rg.AddPass<TransparentPassData>("TransparentPass",
            [&](TransparentPassData& data, RG::RenderPassBuilder& builder)
            {
                data.color = builder.Write(sceneColor, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE);
                data.id    = builder.Write(entityID,   VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE);
                data.depth = builder.WriteDepth(sceneDepth, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE, {});
                if (fogResolved.IsValid())
                    data.fog = builder.Read(fogResolved);
                data.indirect = builder.ReadIndirectBuffer(indirectBufferHandle);
                data.order    = order;
                data.count    = n;
                data.fogValid = fogResolved.IsValid();
                outputHandle  = data.color;
            },
            [this](TransparentPassData& data, RG::RenderPassContext& ctx)
            {
                VkCommandBuffer cmd = ctx.commandBuffer;
                auto& sys = m_Pipeline->GetSystem();
                const auto& draws = sys.GetDrawList().transparent;

                VkPolygonMode polyMode = (sys.GetShadeMode() == ShadeMode::Wireframe)
                                       ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
                sys.GetFrameDebugger().BeginCapturePass(ctx.passIndex, "TransparentPass", "SceneColor", false,
                    { "pbr_transparent", 0, VK_CULL_MODE_BACK_BIT, polyMode, false, true, true, true });

                auto shader = ShaderLibrary::Get("pbr_transparent.frag");
                if (!shader || data.count == 0) { sys.GetFrameDebugger().EndCapturePass(); return; }
                const UUID fragUUID = shader->Handle;

                auto& geo = m_Pipeline->GetGeometry();
                Material::CullMode currentCull = Material::CullMode::Back;
                bool currentSkinned = false;
                auto* pipeline = m_SortedPm.GetOrCreate(fragUUID, Material::RenderMode::Transparent,
                    currentCull, polyMode, geo.GetPBRVertSpv(), m_TransparentFragSpv);
                if (!pipeline) { sys.GetFrameDebugger().EndCapturePass(); return; }
                pipeline->Bind(cmd);

                const u32 slot = static_cast<u32>(Renderer::GetFrameData()->GetRenderFrameIndex()) % MAX_FRAMES_IN_FLIGHT;
                VkDescriptorSet sets[] = {
                    m_Pipeline->GetCurrentViewResources()->globalDescriptorSet[slot],
                    VulkanContext::Get().GetBindlessSet().GetSet(),
                    MaterialSystem::GetDescriptorSet(slot),
                    m_Pipeline->GetLighting().GetLightDescSet(slot),
                    BoneMatrixBuffer::GetDescriptorSet(slot),
                    geo.GetObjectSSBODescSet(slot),
                    m_Pipeline->GetCurrentViewResources()->transparentDescSet[slot],
                };
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    pipeline->GetLayout(), 0, 7, sets, 0, nullptr);

                TransparentPC pc{};
                pc.geomTable = static_cast<u64>(m_Pipeline->GetRt().GetGeometryTableBDA());
                pc.flags     = data.fogValid ? 1u : 0u;
                vkCmdPushConstants(cmd, pipeline->GetLayout(), VK_SHADER_STAGE_FRAGMENT_BIT,
                    0, sizeof(TransparentPC), &pc);

                RG::RenderGraph::ResourceNode* res = (RG::RenderGraph::ResourceNode*)ctx.GetResource(data.color);
                VkViewport viewport{};
                viewport.width    = (float)res->desc.width;
                viewport.height   = (float)res->desc.height;
                viewport.maxDepth = 1.0f;
                vkCmdSetViewport(cmd, 0, 1, &viewport);
                VkRect2D scissor{};
                scissor.extent = { res->desc.width, res->desc.height };
                vkCmdSetScissor(cmd, 0, 1, &scissor);

                const auto& indirectRegion = geo.GetIndirectRegion();
                for (u32 k = 0; k < data.count; ++k)
                {
                    const DrawCommand& dc = draws[data.order[k]];

                    if (dc.cullMode != currentCull || dc.isSkinned != currentSkinned)
                    {
                        currentCull    = dc.cullMode;
                        currentSkinned = dc.isSkinned;
                        VKPipeline* newPipeline = currentSkinned
                            ? m_SortedSkinnedPm.GetOrCreate(fragUUID, Material::RenderMode::Transparent,
                                  currentCull, polyMode, geo.GetPBRSkinnedVertSpv(), m_TransparentFragSpv)
                            : m_SortedPm.GetOrCreate(fragUUID, Material::RenderMode::Transparent,
                                  currentCull, polyMode, geo.GetPBRVertSpv(), m_TransparentFragSpv);
                        if (!newPipeline) continue;
                        newPipeline->Bind(cmd);
                        vkCmdPushConstants(cmd, newPipeline->GetLayout(), VK_SHADER_STAGE_FRAGMENT_BIT,
                            0, sizeof(TransparentPC), &pc);
                    }

                    auto mesh = dc.model->GetMesh(dc.meshIndex);
                    if (!mesh) continue;
                    auto vb = std::static_pointer_cast<VKVertexBuffer>(mesh->GetVertexBuffer());
                    auto ib = std::static_pointer_cast<VKIndexBuffer >(mesh->GetIndexBuffer ());
                    if (!vb || !ib) continue;

                    VkBuffer vbuf[] = { vb->GetVulkanBuffer() };
                    VkDeviceSize offsets[] = { 0 };
                    vkCmdBindVertexBuffers(cmd, 0, 1, vbuf, offsets);
                    vkCmdBindIndexBuffer(cmd, ib->GetVulkanBuffer(), 0, VK_INDEX_TYPE_UINT32);

                    // GPU cull zeroed instanceCount for off-frustum draws; the indirect slot is keyed
                    // by gpuObjectIndex, so sorted iteration alone reorders submission.
                    const u32 viewBaseRegion = m_Pipeline->GetCurrentView()->viewIndex * RenderPipeline::k_IndirectRegionsPerView;
                    const u32 cmdIndex = viewBaseRegion * RenderPipeline::k_IndirectRegionStride + dc.gpuObjectIndex;
                    VkDeviceSize indirectOffset = indirectRegion.offset + cmdIndex * sizeof(VkDrawIndexedIndirectCommand);
                    vkCmdDrawIndexedIndirect(cmd, indirectRegion.buffer, indirectOffset, 1,
                        sizeof(VkDrawIndexedIndirectCommand));

                    if (sys.GetFrameDebugger().state == DebuggerState::CaptureRequested)
                    {
                        std::string entName = "Entity";
                        const auto& tags = sys.GetActiveSnapshot().tagsByEntity;
                        u32 idx = entt::to_entity(dc.entity);
                        if (idx < tags.size() && tags[idx])
                            entName = tags[idx];
                        u32 vkCull = (currentCull == Material::CullMode::Back) ? VK_CULL_MODE_BACK_BIT
                                   : (currentCull == Material::CullMode::Front) ? VK_CULL_MODE_FRONT_BIT
                                   : VK_CULL_MODE_NONE;
                        sys.GetFrameDebugger().CaptureIndirectDraw("TransparentPass",
                            dc.model->GetName() + "[" + std::to_string(dc.meshIndex) + "]",
                            entName, dc.entityIndex, ib->GetCount(),
                            dc.gpuObjectIndex, indirectOffset,
                            { "pbr_transparent", static_cast<u32>(Material::RenderMode::Transparent),
                              vkCull, polyMode, currentSkinned, true, true, true });
                    }
                }

                sys.GetFrameDebugger().EndCapturePass();
            }
        );
        return outputHandle;
    }
}
