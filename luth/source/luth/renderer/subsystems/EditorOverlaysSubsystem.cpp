#include "luthpch.h"
#include "luth/renderer/subsystems/EditorOverlaysSubsystem.h"
#include "luth/renderer/subsystems/LightingSubsystem.h"
#include "luth/renderer/RenderPipeline.h"
#include "luth/renderer/Renderer.h"
#include "luth/renderer/FrameTargets.h"
#include "luth/scene/systems/RenderingSystem.h"
#include "luth/scene/Entity.h"
#include "luth/renderer/material/Material.h"
#include "luth/renderer/material/MaterialSystem.h"
#include "luth/renderer/resources/BoneMatrixBuffer.h"
#include "luth/renderer/resources/Buffer.h"
#include "luth/renderer/resources/Model.h"
#include "luth/renderer/draw/DrawCommand.h"
#include "luth/renderer/shader/ShaderLibrary.h"
#include "luth/renderer/backend/vulkan/VulkanContext.h"
#include "luth/renderer/backend/vulkan/VulkanTexture.h"
#include "luth/renderer/backend/vulkan/VulkanBuffer.h"

namespace Luth
{
    namespace {
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
        std::pair<std::vector<VkVertexInputBindingDescription>, std::vector<VkVertexInputAttributeDescription>>
        MakePositionOnlyWithFullStride() {
            BufferLayout layout = { { ShaderDataType::Float3, "a_Position" } };
            auto bindings = layout.GetBindingDescriptions();
            auto attribs  = layout.GetAttributeDescriptions();
            if (!bindings.empty()) bindings[0].stride = sizeof(float) * (3 + 3 + 2 + 2 + 3);
            return { std::move(bindings), std::move(attribs) };
        }
    }

    void EditorOverlaysSubsystem::Init(RenderPipeline& pipeline)
    {
        m_Pipeline = &pipeline;

        auto loadSpv = [](const char* relPath) -> std::vector<u32> {
            auto sh = ShaderLibrary::LoadEngine(relPath);
            return sh ? sh->GetSpirV() : std::vector<u32>{};
        };
        m_SelectionMaskVertSpv        = loadSpv("shaders/selectionMask.vert");
        m_SelectionMaskFragSpv        = loadSpv("shaders/selectionMask.frag");
        m_SelectionMaskSkinnedVertSpv = loadSpv("shaders/selectionMask_skinned.vert");
        m_OutlineFragSpv              = loadSpv("shaders/outline.frag");
        m_GridFragSpv                 = loadSpv("shaders/grid.frag");
        m_FullscreenVertSpv           = loadSpv("shaders/fullscreen.vert");

        if (m_SelectionMaskVertSpv.empty() || m_SelectionMaskFragSpv.empty() ||
            m_SelectionMaskSkinnedVertSpv.empty() ||
            m_OutlineFragSpv.empty() || m_GridFragSpv.empty() || m_FullscreenVertSpv.empty())
        {
            LH_CORE_ERROR("EditorOverlaysSubsystem: shader SPIR-V empty after asset load!");
            return;
        }

        CreateLayouts();
    }

    void EditorOverlaysSubsystem::CreateLayouts()
    {
        VkDevice device = VulkanContext::Get().GetDevice();

        // Outline: 3 combined image samplers (mask, selection depth, scene depth).
        {
            VkSamplerCreateInfo si{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
            si.magFilter    = VK_FILTER_NEAREST;
            si.minFilter    = VK_FILTER_NEAREST;
            si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            si.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
            vkCreateSampler(device, &si, nullptr, &m_OutlineSampler);

            VkDescriptorSetLayoutBinding bindings[3] = {};
            for (u32 i = 0; i < 3; ++i) {
                bindings[i].binding = i;
                bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                bindings[i].descriptorCount = 1;
                bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
            }
            VkDescriptorSetLayoutCreateInfo li{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
            li.bindingCount = 3;
            li.pBindings    = bindings;
            vkCreateDescriptorSetLayout(device, &li, nullptr, &m_OutlineDescSetLayout);
        }

        // Grid: binding 0 = per-view GlobalUBO (shared with Set 0 binding 0; rebound by GlobalSubsystem),
        //       binding 1 = scene depth sampler.
        {
            VkSamplerCreateInfo si{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
            si.magFilter    = VK_FILTER_NEAREST;
            si.minFilter    = VK_FILTER_NEAREST;
            si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            si.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
            vkCreateSampler(device, &si, nullptr, &m_GridDepthSampler);

            VkDescriptorSetLayoutBinding bindings[2] = {};
            bindings[0].binding = 0;
            bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            bindings[0].descriptorCount = 1;
            bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
            bindings[1].binding = 1;
            bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            bindings[1].descriptorCount = 1;
            bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

            // invariant: binding 0 (per-view GlobalUBO) shares lifetime AND slot with
            // Set 0 binding 0 — rewritten per render-stage; cycling alone doesn't
            // avoid the in-pending-cmdbuf race. UAB needed (validation 03047).
            VkDescriptorBindingFlags bindingFlags[2] = {
                VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT,
                VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT,
            };
            VkDescriptorSetLayoutBindingFlagsCreateInfo bindingFlagsCI{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO };
            bindingFlagsCI.bindingCount  = 2;
            bindingFlagsCI.pBindingFlags = bindingFlags;

            VkDescriptorSetLayoutCreateInfo li{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
            li.pNext        = &bindingFlagsCI;
            li.flags        = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
            li.bindingCount = 2;
            li.pBindings    = bindings;
            vkCreateDescriptorSetLayout(device, &li, nullptr, &m_GridDescSetLayout);
        }
    }

    void EditorOverlaysSubsystem::BuildPipelines(const std::vector<VkDescriptorSetLayout>& geoLayouts)
    {
        BuildSelectionPipelines(geoLayouts);
        BuildOutlinePipeline();
        BuildGridPipeline();
    }

    void EditorOverlaysSubsystem::BuildSelectionPipelines(const std::vector<VkDescriptorSetLayout>& geoLayouts)
    {
        // Selection mask uses Sets 0-4 (no Set 5 / no GPUObject SSBO — uses ObjectPushConstants).
        std::vector<VkDescriptorSetLayout> layouts(geoLayouts.begin(),
                                                   geoLayouts.begin() + std::min<size_t>(5, geoLayouts.size()));

        VkPushConstantRange pcRange{};
        pcRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        pcRange.offset = 0;
        pcRange.size = sizeof(ObjectPushConstants);

        auto [posBindings, posAttribs] = MakePositionOnlyWithFullStride();

        if (!m_SelectionMaskVertSpv.empty() && !m_SelectionMaskFragSpv.empty())
        {
            PipelineConfig cfg;
            cfg.colorFormats = { VK_FORMAT_R8G8B8A8_UNORM };
            cfg.depthFormat  = VK_FORMAT_D32_SFLOAT;
            cfg.depthTest    = true; cfg.depthWrite = true;
            cfg.blendEnabled = false;
            cfg.cullMode     = VK_CULL_MODE_BACK_BIT;
            cfg.frontFace    = VK_FRONT_FACE_COUNTER_CLOCKWISE;
            cfg.bindingDescriptions   = posBindings;
            cfg.attributeDescriptions = posAttribs;
            cfg.pushConstantRanges    = { pcRange };
            m_SelectionMaskPipeline = std::make_unique<VKPipeline>(
                cfg, m_SelectionMaskVertSpv, m_SelectionMaskFragSpv, layouts);
        }

        if (!m_SelectionMaskSkinnedVertSpv.empty() && !m_SelectionMaskFragSpv.empty())
        {
            auto skinned = MakeSkinnedVertexLayout();
            auto skinnedBindings = skinned.GetBindingDescriptions();
            auto skinnedAttribs  = skinned.GetAttributeDescriptions();

            PipelineConfig cfg;
            cfg.colorFormats = { VK_FORMAT_R8G8B8A8_UNORM };
            cfg.depthFormat  = VK_FORMAT_D32_SFLOAT;
            cfg.depthTest    = true; cfg.depthWrite = true;
            cfg.blendEnabled = false;
            cfg.cullMode     = VK_CULL_MODE_BACK_BIT;
            cfg.frontFace    = VK_FRONT_FACE_COUNTER_CLOCKWISE;
            cfg.bindingDescriptions   = skinnedBindings;
            cfg.attributeDescriptions = skinnedAttribs;
            cfg.pushConstantRanges    = { pcRange };
            m_SelectionMaskSkinnedPipeline = std::make_unique<VKPipeline>(
                cfg, m_SelectionMaskSkinnedVertSpv, m_SelectionMaskFragSpv, layouts);
        }
    }

    void EditorOverlaysSubsystem::BuildOutlinePipeline()
    {
        if (m_FullscreenVertSpv.empty() || m_OutlineFragSpv.empty() || m_OutlineDescSetLayout == VK_NULL_HANDLE) return;

        std::vector<VkDescriptorSetLayout> layouts = { m_OutlineDescSetLayout };

        VkPushConstantRange pcRange{};
        pcRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        pcRange.offset = 0;
        pcRange.size = sizeof(float) * 8;

        PipelineConfig cfg;
        cfg.colorFormats = { VK_FORMAT_R8G8B8A8_UNORM };
        cfg.depthFormat  = VK_FORMAT_UNDEFINED;
        cfg.depthTest    = false; cfg.depthWrite = false;
        cfg.blendEnabled = true; // alpha-blend the outline on top
        cfg.cullMode     = VK_CULL_MODE_NONE;
        cfg.pushConstantRanges = { pcRange };
        m_OutlinePipeline = std::make_unique<VKPipeline>(
            cfg, m_FullscreenVertSpv, m_OutlineFragSpv, layouts);
    }

    void EditorOverlaysSubsystem::BuildGridPipeline()
    {
        if (m_FullscreenVertSpv.empty() || m_GridFragSpv.empty() || m_GridDescSetLayout == VK_NULL_HANDLE) return;

        std::vector<VkDescriptorSetLayout> layouts = { m_GridDescSetLayout };

        VkPushConstantRange pcRange{};
        pcRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        pcRange.offset = 0;
        pcRange.size = sizeof(float) * 16;

        PipelineConfig cfg;
        cfg.colorFormats = { VK_FORMAT_R16G16B16A16_SFLOAT };
        cfg.depthFormat  = VK_FORMAT_UNDEFINED;
        cfg.depthTest    = false; cfg.depthWrite = false;
        cfg.blendEnabled = true; // alpha-composite grid over scene
        cfg.cullMode     = VK_CULL_MODE_NONE;
        cfg.pushConstantRanges = { pcRange };
        m_GridPipeline = std::make_unique<VKPipeline>(
            cfg, m_FullscreenVertSpv, m_GridFragSpv, layouts);
    }

    void EditorOverlaysSubsystem::Shutdown()
    {
        VkDevice device = VulkanContext::Get().GetDevice();
        m_GridPipeline.reset();
        m_OutlinePipeline.reset();
        m_SelectionMaskSkinnedPipeline.reset();
        m_SelectionMaskPipeline.reset();
        if (m_GridDepthSampler)     { vkDestroySampler(device, m_GridDepthSampler, nullptr); m_GridDepthSampler = VK_NULL_HANDLE; }
        if (m_GridDescSetLayout)    { vkDestroyDescriptorSetLayout(device, m_GridDescSetLayout, nullptr); m_GridDescSetLayout = VK_NULL_HANDLE; }
        if (m_OutlineSampler)       { vkDestroySampler(device, m_OutlineSampler, nullptr); m_OutlineSampler = VK_NULL_HANDLE; }
        if (m_OutlineDescSetLayout) { vkDestroyDescriptorSetLayout(device, m_OutlineDescSetLayout, nullptr); m_OutlineDescSetLayout = VK_NULL_HANDLE; }
    }

    bool EditorOverlaysSubsystem::OnShaderReloaded(const std::string& name, const std::vector<u32>& spv,
                                                    const std::vector<VkDescriptorSetLayout>& geoLayouts)
    {
        auto deferGfx = [](std::unique_ptr<VKPipeline>& p) {
            if (auto* raw = p.release(); raw)
                VulkanContext::Get().PushDeletion([raw]() { delete raw; });
        };

        if      (name == "selectionMask.vert")         m_SelectionMaskVertSpv        = spv;
        else if (name == "selectionMask.frag")         m_SelectionMaskFragSpv        = spv;
        else if (name == "selectionMask_skinned.vert") m_SelectionMaskSkinnedVertSpv = spv;
        else if (name == "outline.frag")               m_OutlineFragSpv              = spv;
        else if (name == "grid.frag")                  m_GridFragSpv                 = spv;
        else if (name == "fullscreen.vert")            m_FullscreenVertSpv           = spv;
        else return false;

        if (name == "outline.frag" || name == "grid.frag" || name == "fullscreen.vert")
        {
            deferGfx(m_OutlinePipeline);
            deferGfx(m_GridPipeline);
            BuildOutlinePipeline();
            BuildGridPipeline();
        }
        if (name == "selectionMask.vert" || name == "selectionMask.frag" || name == "selectionMask_skinned.vert")
        {
            deferGfx(m_SelectionMaskPipeline);
            deferGfx(m_SelectionMaskSkinnedPipeline);
            BuildSelectionPipelines(geoLayouts);
        }
        return true;
    }

    void EditorOverlaysSubsystem::WriteOutlineView(ViewResources& vr, FrameTargets& targets)
    {
        if (vr.outlineDescSet == VK_NULL_HANDLE || m_OutlineSampler == VK_NULL_HANDLE) return;

        VkDevice device = VulkanContext::Get().GetDevice();

        auto vkMask     = std::static_pointer_cast<VKTexture>(targets.GetSelectionMask());
        auto vkSelDepth = std::static_pointer_cast<VKTexture>(targets.GetSelectionDepth());
        auto vkScnDepth = std::static_pointer_cast<VKTexture>(targets.GetSceneDepth());

        VkDescriptorImageInfo maskInfo{};
        maskInfo.sampler     = m_OutlineSampler;
        maskInfo.imageView   = vkMask->GetImageView();
        maskInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkDescriptorImageInfo selDepthInfo{};
        selDepthInfo.sampler     = m_OutlineSampler;
        selDepthInfo.imageView   = vkSelDepth->GetImageView();
        selDepthInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkDescriptorImageInfo scnDepthInfo{};
        scnDepthInfo.sampler     = m_OutlineSampler;
        scnDepthInfo.imageView   = vkScnDepth->GetImageView();
        scnDepthInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkWriteDescriptorSet writes[3] = {};
        writes[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        writes[0].dstSet          = vr.outlineDescSet;
        writes[0].dstBinding      = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[0].pImageInfo      = &maskInfo;
        writes[1] = writes[0];
        writes[1].dstBinding      = 1;
        writes[1].pImageInfo      = &selDepthInfo;
        writes[2] = writes[0];
        writes[2].dstBinding      = 2;
        writes[2].pImageInfo      = &scnDepthInfo;

        vkUpdateDescriptorSets(device, 3, writes, 0, nullptr);
    }

    void EditorOverlaysSubsystem::WriteGridView(ViewResources& vr, FrameTargets& targets)
    {
        if (vr.gridDescSet[0] == VK_NULL_HANDLE || m_GridDepthSampler == VK_NULL_HANDLE) return;

        VkDevice device = VulkanContext::Get().GetDevice();

        // Binding 0 (per-view GlobalUBO) rewritten per render-stage by GlobalSubsystem::UpdateUBO.
        // Stable depth-sampler binding propagated to every cycled slot.
        auto vkScnDepth = std::static_pointer_cast<VKTexture>(targets.GetSceneDepth());
        VkDescriptorImageInfo depthInfo{};
        depthInfo.sampler     = m_GridDepthSampler;
        depthInfo.imageView   = vkScnDepth->GetImageView();
        depthInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkWriteDescriptorSet samplerWrites[MAX_FRAMES_IN_FLIGHT] = {};
        for (u32 s = 0; s < MAX_FRAMES_IN_FLIGHT; ++s)
        {
            samplerWrites[s] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            samplerWrites[s].dstSet          = vr.gridDescSet[s];
            samplerWrites[s].dstBinding      = 1;
            samplerWrites[s].descriptorCount = 1;
            samplerWrites[s].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            samplerWrites[s].pImageInfo      = &depthInfo;
        }

        vkUpdateDescriptorSets(device, MAX_FRAMES_IN_FLIGHT, samplerWrites, 0, nullptr);
    }

    void EditorOverlaysSubsystem::CollectSelectedHandles(const std::vector<Entity>& selected, std::unordered_set<entt::entity>& outHandles) const
    {
        for (const auto& entity : selected)
        {
            if (!entity || !entity.IsValid()) continue;
            outHandles.insert((entt::entity)entity);
            // Recursively include children so outline wraps entire subtrees.
            for (const auto& child : entity.GetChildren())
            {
                std::vector<Entity> childVec = { child };
                CollectSelectedHandles(childVec, outHandles);
            }
        }
    }

    SelectionMaskOutput EditorOverlaysSubsystem::AddSelectionMaskPass(RG::RenderGraph& rg)
    {
        struct SelectionMaskPassData {
            RG::ResourceHandle maskTex;
            RG::ResourceHandle depthTex;
        };
        SelectionMaskOutput output;

        rg.AddPass<SelectionMaskPassData>("SelectionMaskPass",
            [&](SelectionMaskPassData& data, RG::RenderPassBuilder& builder)
            {
                const auto* view = m_Pipeline->GetCurrentView();
                auto vkMask = std::static_pointer_cast<VKTexture>(view->targets->GetSelectionMask());
                RG::TextureDesc maskDesc;
                maskDesc.name   = "SelectionMask";
                maskDesc.width  = view->targets->GetSelectionMask()->GetWidth();
                maskDesc.height = view->targets->GetSelectionMask()->GetHeight();
                maskDesc.format = RG::TextureFormat::RGBA8_Unorm;

                data.maskTex = rg.ImportResource(maskDesc,
                    (void*)vkMask->GetImage(), (void*)vkMask->GetImageView(),
                    RG::ResourceState::Undefined);

                VkClearValue colorClear{};
                colorClear.color = {{0.0f, 0.0f, 0.0f, 0.0f}};
                data.maskTex = builder.Write(data.maskTex,
                    VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, colorClear);

                auto vkDepth = std::static_pointer_cast<VKTexture>(view->targets->GetSelectionDepth());
                RG::TextureDesc depthDesc;
                depthDesc.name   = "SelectionDepth";
                depthDesc.width  = view->targets->GetSelectionDepth()->GetWidth();
                depthDesc.height = view->targets->GetSelectionDepth()->GetHeight();
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
            [this](SelectionMaskPassData& data, RG::RenderPassContext& ctx)
            {
                auto& sys = m_Pipeline->GetSystem();
                const auto* view = m_Pipeline->GetCurrentView();
                ViewResources* vr = m_Pipeline->GetCurrentViewResources();

                sys.GetFrameDebugger().BeginCapturePass(ctx.passIndex, "SelectionMaskPass", "SelectionMask", false,
                    { "selectionMask", 0, VK_CULL_MODE_BACK_BIT, VK_POLYGON_MODE_FILL, false, true, true, false });

                if (!m_SelectionMaskPipeline) { sys.GetFrameDebugger().EndCapturePass(); return; }

                std::unordered_set<entt::entity> selectedSet;
                CollectSelectedHandles(view->camera.selectedEntities, selectedSet);
                if (selectedSet.empty()) return;

                VkCommandBuffer cmd = ctx.commandBuffer;
                const u32 slot = static_cast<u32>(Renderer::GetFrameData()->GetRenderFrameIndex()) % MAX_FRAMES_IN_FLIGHT;
                VkDescriptorSet bindlessSet = VulkanContext::Get().GetBindlessSet().GetSet();
                VkDescriptorSet sets[] = {
                    vr->globalDescriptorSet[slot],
                    bindlessSet,
                    MaterialSystem::GetDescriptorSet(slot),
                    m_Pipeline->GetLighting().GetLightDescSet(slot),
                    BoneMatrixBuffer::GetDescriptorSet(slot)
                };

                m_SelectionMaskPipeline->Bind(cmd);
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    m_SelectionMaskPipeline->GetLayout(), 0, 5, sets, 0, nullptr);

                u32 w = view->targets->GetSelectionMask()->GetWidth();
                u32 h = view->targets->GetSelectionMask()->GetHeight();
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

                        if (sys.GetFrameDebugger().state == DebuggerState::CaptureRequested)
                        {
                            std::string entName = "Entity";
                            const auto& tags = sys.GetActiveSnapshot().tagsByEntity;
                            u32 idx = entt::to_entity(dc.entity);
                            if (idx < tags.size() && tags[idx])
                                entName = tags[idx];
                            sys.GetFrameDebugger().CaptureDrawCall("SelectionMaskPass",
                                dc.model->GetName() + "[" + std::to_string(dc.meshIndex) + "]",
                                entName, dc.entityIndex, ib->GetCount(), pc,
                                { "selectionMask", 0, VK_CULL_MODE_BACK_BIT, VK_POLYGON_MODE_FILL,
                                  currentSkinned, true, true, false });
                        }
                    }
                };

                DrawBatch(sys.GetDrawList().opaque);
                DrawBatch(sys.GetDrawList().cutout);
                DrawBatch(sys.GetDrawList().transparent);

                sys.GetFrameDebugger().EndCapturePass();
            }
        );

        return output;
    }

    RG::ResourceHandle EditorOverlaysSubsystem::AddOutlinePass(
        RG::RenderGraph& rg, RG::ResourceHandle ldrOutput, SelectionMaskOutput maskOutput, RG::ResourceHandle sceneDepth)
    {
        const auto* view = m_Pipeline->GetCurrentView();
        if (!m_OutlinePipeline || !view->targets->GetLDROutput()) return ldrOutput;

        struct OutlinePassData {
            RG::ResourceHandle output;
            RG::ResourceHandle maskInput;
            RG::ResourceHandle selDepthInput;
            RG::ResourceHandle scnDepthInput;
        };
        RG::ResourceHandle outputHandle;

        rg.AddPass<OutlinePassData>("OutlinePass",
            [&, ldrOutput, maskOutput, sceneDepth](OutlinePassData& data, RG::RenderPassBuilder& builder)
            {
                data.output = builder.Write(ldrOutput,
                    VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE);
                data.maskInput     = builder.Read(maskOutput.mask);
                data.selDepthInput = builder.Read(maskOutput.depth);
                data.scnDepthInput = builder.Read(sceneDepth);
                outputHandle = data.output;
            },
            [this](OutlinePassData& data, RG::RenderPassContext& ctx)
            {
                auto& sys = m_Pipeline->GetSystem();
                const auto* view = m_Pipeline->GetCurrentView();
                ViewResources* vr = m_Pipeline->GetCurrentViewResources();

                sys.GetFrameDebugger().BeginCapturePass(ctx.passIndex, "OutlinePass", "LDROutput", false,
                    { "outline", 0, VK_CULL_MODE_NONE, VK_POLYGON_MODE_FILL, false, false, false, true });

                VkCommandBuffer cmd = ctx.commandBuffer;
                m_OutlinePipeline->Bind(cmd);
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    m_OutlinePipeline->GetLayout(), 0, 1, &vr->outlineDescSet, 0, nullptr);

                u32 w = view->targets->GetLDROutput()->GetWidth();
                u32 h = view->targets->GetLDROutput()->GetHeight();
                VkViewport vp{}; vp.width = (float)w; vp.height = (float)h; vp.maxDepth = 1.0f;
                vkCmdSetViewport(cmd, 0, 1, &vp);
                VkRect2D sc{}; sc.extent = { w, h };
                vkCmdSetScissor(cmd, 0, 1, &sc);

                // Push constants flow EditorSettings → EditorViewportState → CameraParams (App.cpp).
                const auto& cp = sys.GetCameraParams();
                struct OutlinePushConstants {
                    float outlineWidth;
                    float texelSizeX, texelSizeY;
                    float outlineColorR, outlineColorG, outlineColorB, outlineColorA;
                    float occludedAlpha;
                } pc;
                pc.outlineWidth     = cp.outlineWidth;
                pc.texelSizeX       = 1.0f / (float)w;
                pc.texelSizeY       = 1.0f / (float)h;
                pc.outlineColorR    = cp.outlineColor.r;
                pc.outlineColorG    = cp.outlineColor.g;
                pc.outlineColorB    = cp.outlineColor.b;
                pc.outlineColorA    = cp.outlineColor.a;
                pc.occludedAlpha    = cp.outlineOccludedAlpha;
                vkCmdPushConstants(cmd, m_OutlinePipeline->GetLayout(),
                    VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);

                vkCmdDraw(cmd, 3, 1, 0, 0);

                ObjectPushConstants dummyPC{};
                sys.GetFrameDebugger().CaptureDrawCall("OutlinePass", "FullscreenTriangle", "OutlinePass", 0, 0, dummyPC,
                    { "outline", 0, VK_CULL_MODE_NONE, VK_POLYGON_MODE_FILL, false, false, false, true });
                sys.GetFrameDebugger().EndCapturePass();
            }
        );
        return outputHandle;
    }

    RG::ResourceHandle EditorOverlaysSubsystem::AddGridPass(RG::RenderGraph& rg, RG::ResourceHandle sceneColor, RG::ResourceHandle sceneDepth)
    {
        if (!m_GridPipeline) return sceneColor;

        struct GridPassData {
            RG::ResourceHandle colorTex;
            RG::ResourceHandle depthInput;
        };
        RG::ResourceHandle outputHandle;

        rg.AddPass<GridPassData>("GridPass",
            [&, sceneColor, sceneDepth](GridPassData& data, RG::RenderPassBuilder& builder)
            {
                data.colorTex   = builder.Write(sceneColor,
                    VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE);
                data.depthInput = builder.Read(sceneDepth);
                outputHandle = data.colorTex;
            },
            [this](GridPassData& data, RG::RenderPassContext& ctx)
            {
                auto& sys = m_Pipeline->GetSystem();
                ViewResources* vr = m_Pipeline->GetCurrentViewResources();

                sys.GetFrameDebugger().BeginCapturePass(ctx.passIndex, "GridPass", "SceneColor", false,
                    { "grid", 0, VK_CULL_MODE_NONE, VK_POLYGON_MODE_FILL, false, false, false, true });

                VkCommandBuffer cmd = ctx.commandBuffer;
                const u32 slot = static_cast<u32>(Renderer::GetFrameData()->GetRenderFrameIndex()) % MAX_FRAMES_IN_FLIGHT;
                m_GridPipeline->Bind(cmd);
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    m_GridPipeline->GetLayout(), 0, 1, &vr->gridDescSet[slot], 0, nullptr);

                RG::RenderGraph::ResourceNode* res = (RG::RenderGraph::ResourceNode*)ctx.GetResource(data.colorTex);
                VkViewport vp{};
                vp.width  = (float)res->desc.width;
                vp.height = (float)res->desc.height;
                vp.maxDepth = 1.0f;
                vkCmdSetViewport(cmd, 0, 1, &vp);
                VkRect2D sc{}; sc.extent = { res->desc.width, res->desc.height };
                vkCmdSetScissor(cmd, 0, 1, &sc);

                const auto& cp = sys.GetCameraParams();
                struct GridPushConstants {
                    float axisXColor[4];
                    float axisZColor[4];
                    float gridColor[4];
                    float majorScale;
                    float fadeStart;
                    float fadeEnd;
                    float lineThickness;
                } gpc{};
                gpc.axisXColor[0] = cp.gridAxisXColor.r; gpc.axisXColor[1] = cp.gridAxisXColor.g; gpc.axisXColor[2] = cp.gridAxisXColor.b; gpc.axisXColor[3] = cp.gridAxisXColor.a;
                gpc.axisZColor[0] = cp.gridAxisZColor.r; gpc.axisZColor[1] = cp.gridAxisZColor.g; gpc.axisZColor[2] = cp.gridAxisZColor.b; gpc.axisZColor[3] = cp.gridAxisZColor.a;
                gpc.gridColor[0]  = cp.gridColor.r;      gpc.gridColor[1]  = cp.gridColor.g;      gpc.gridColor[2]  = cp.gridColor.b;      gpc.gridColor[3]  = cp.gridColor.a;
                gpc.majorScale    = cp.gridMajorScale;
                gpc.fadeStart     = cp.gridFadeStart;
                gpc.fadeEnd       = cp.gridFadeEnd;
                gpc.lineThickness = cp.gridLineThickness;
                vkCmdPushConstants(cmd, m_GridPipeline->GetLayout(),
                    VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(gpc), &gpc);

                vkCmdDraw(cmd, 3, 1, 0, 0);

                ObjectPushConstants dummyPC{};
                sys.GetFrameDebugger().CaptureDrawCall("GridPass", "FullscreenTriangle", "GridPass", 0, 0, dummyPC,
                    { "grid", 0, VK_CULL_MODE_NONE, VK_POLYGON_MODE_FILL, false, false, false, true });
                sys.GetFrameDebugger().EndCapturePass();
            }
        );
        return outputHandle;
    }
}
