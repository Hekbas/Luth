#include "luthpch.h"
#include "luth/renderer/subsystems/RtRestirSubsystem.h"
#include "luth/renderer/subsystems/RtSubsystem.h"
#include "luth/renderer/RenderPipeline.h"
#include "luth/renderer/Renderer.h"
#include "luth/renderer/FrameTargets.h"
#include "luth/renderer/shader/ShaderLibrary.h"
#include "luth/renderer/backend/vulkan/VulkanContext.h"
#include "luth/renderer/backend/vulkan/VulkanTexture.h"
#include "luth/core/FrameData.h"
#include "luth/core/types/LuthMath.h"

namespace Luth
{
    namespace {
        // Sizes the reservoir allocation only — the GPU layout lives in restir_common.glsl's
        // Reservoir struct; any field change must update both. see arch/rendering-pipeline.md
        struct GPUReservoir {
            u32 lightIndex;
            f32 W, wSum;
            u32 M;
            f32 targetPdf;
            f32 pad0, pad1, pad2;
        };
        static_assert(sizeof(GPUReservoir) == 32, "GPUReservoir must match restir_common.glsl Reservoir (32 B)");

        struct RestirPC {
            Mat4 invViewProj;
            u32  candidateCount;
            u32  frameSeed;
            u32  pad0;
            u32  pad1;
        };
        static_assert(sizeof(RestirPC) == 80, "RestirPC must be 80 B (matches restir_*.comp push_constant)");

        constexpr u32 k_DefaultCandidateCount = 32;
    }

    void RtRestirSubsystem::Init(RenderPipeline& pipeline)
    {
        m_Pipeline = &pipeline;
        VkDevice device = VulkanContext::Get().GetDevice();

        // Linear clamp-to-edge — same shape as RtSubsystem's pass sampler for SceneDepth/SlimNormal.
        VkSamplerCreateInfo sampCI{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        sampCI.magFilter    = VK_FILTER_LINEAR;
        sampCI.minFilter    = VK_FILTER_LINEAR;
        sampCI.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampCI.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampCI.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampCI.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        vkCreateSampler(device, &sampCI, nullptr, &m_Sampler);

        // Set 2 (pass-local): b0 depth sampler, b1 slimNormal sampler, b2 reservoir SSBO, b3 DI storage image.
        VkDescriptorSetLayoutBinding bindings[4]{};
        bindings[0].binding         = 0;
        bindings[0].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[0].descriptorCount = 1;
        bindings[0].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
        bindings[1].binding         = 1;
        bindings[1].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[1].descriptorCount = 1;
        bindings[1].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
        bindings[2].binding         = 2;
        bindings[2].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[2].descriptorCount = 1;
        bindings[2].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
        bindings[3].binding         = 3;
        bindings[3].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        bindings[3].descriptorCount = 1;
        bindings[3].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;

        VkDescriptorSetLayoutCreateInfo layoutCI{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        layoutCI.bindingCount = 4;
        layoutCI.pBindings    = bindings;
        vkCreateDescriptorSetLayout(device, &layoutCI, nullptr, &m_SetLayout);

        if (auto sh = ShaderLibrary::LoadEngine("shaders/restir_initial.comp"))
            m_InitialSpv = sh->GetSpirV();
        if (auto sh = ShaderLibrary::LoadEngine("shaders/restir_shade.comp"))
            m_ShadeSpv = sh->GetSpirV();
        if (m_InitialSpv.empty() || m_ShadeSpv.empty())
        {
            LH_CORE_ERROR("RtRestirSubsystem: failed to load restir_initial.comp + restir_shade.comp SPIR-V");
            return;
        }

        // Sets: 0 = global (UBO b0 + TLAS b6), 1 = light SSBO, 2 = pass-local. Matches both shaders.
        const std::vector<VkDescriptorSetLayout> layouts = {
            m_Pipeline->GetGlobal().GetSetLayout(),
            m_Pipeline->GetLighting().GetSetLayout(),
            m_SetLayout,
        };
        VkPushConstantRange pcRange{ VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(RestirPC) };

        m_InitialPipeline = std::make_unique<VKComputePipeline>(
            m_InitialSpv, layouts, std::vector<VkPushConstantRange>{ pcRange });
        m_ShadePipeline = std::make_unique<VKComputePipeline>(
            m_ShadeSpv, layouts, std::vector<VkPushConstantRange>{ pcRange });
    }

    void RtRestirSubsystem::Shutdown()
    {
        VkDevice device = VulkanContext::Get().GetDevice();
        m_InitialPipeline.reset();
        m_ShadePipeline.reset();
        if (m_Sampler)   vkDestroySampler(device, m_Sampler, nullptr);
        if (m_SetLayout) vkDestroyDescriptorSetLayout(device, m_SetLayout, nullptr);
        m_Sampler   = VK_NULL_HANDLE;
        m_SetLayout = VK_NULL_HANDLE;
        m_InitialSpv.clear();
        m_ShadeSpv.clear();
        m_Pipeline = nullptr;
    }

    bool RtRestirSubsystem::OnShaderReloaded(const std::string& name, const std::vector<u32>& spv)
    {
        if (m_SetLayout == VK_NULL_HANDLE || !m_Pipeline) return false;

        const bool isInitial = (name == "restir_initial.comp");
        const bool isShade   = (name == "restir_shade.comp");
        if (!isInitial && !isShade) return false;

        const std::vector<VkDescriptorSetLayout> layouts = {
            m_Pipeline->GetGlobal().GetSetLayout(),
            m_Pipeline->GetLighting().GetSetLayout(),
            m_SetLayout,
        };
        VkPushConstantRange pcRange{ VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(RestirPC) };

        auto deferComp = [](std::unique_ptr<VKComputePipeline>& p) {
            if (auto* raw = p.release(); raw)
                VulkanContext::Get().PushDeletion([raw]() { delete raw; });
        };

        if (isInitial)
        {
            m_InitialSpv = spv;
            deferComp(m_InitialPipeline);
            m_InitialPipeline = std::make_unique<VKComputePipeline>(
                m_InitialSpv, layouts, std::vector<VkPushConstantRange>{ pcRange });
        }
        else
        {
            m_ShadeSpv = spv;
            deferComp(m_ShadePipeline);
            m_ShadePipeline = std::make_unique<VKComputePipeline>(
                m_ShadeSpv, layouts, std::vector<VkPushConstantRange>{ pcRange });
        }
        return true;
    }

    void RtRestirSubsystem::WriteView(ViewResources& vr, FrameTargets& targets)
    {
        if (vr.restirDescSet[0] == VK_NULL_HANDLE) return;
        if (!targets.GetSceneDepth() || !targets.GetSlimNormal() || !vr.restirDI || !vr.restirReservoir.buffer) return;

        VkDevice device = VulkanContext::Get().GetDevice();

        const VkImageView depthView  = std::static_pointer_cast<VKTexture>(targets.GetSceneDepth())->GetImageView();
        const VkImageView normalView = std::static_pointer_cast<VKTexture>(targets.GetSlimNormal())->GetImageView();
        const VkImageView diView     = std::static_pointer_cast<VKTexture>(vr.restirDI)->GetImageView();

        VkDescriptorImageInfo depthInfo{};
        depthInfo.sampler     = m_Sampler;
        depthInfo.imageView   = depthView;
        depthInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkDescriptorImageInfo normalInfo{};
        normalInfo.sampler     = m_Sampler;
        normalInfo.imageView   = normalView;
        normalInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkDescriptorBufferInfo reservoirInfo{};
        reservoirInfo.buffer = vr.restirReservoir.buffer;
        reservoirInfo.offset = vr.restirReservoir.offset;
        reservoirInfo.range  = vr.restirReservoir.size;

        VkDescriptorImageInfo diInfo{};
        diInfo.imageView   = diView;
        diInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkWriteDescriptorSet writes[4 * MAX_FRAMES_IN_FLIGHT]{};
        u32 n = 0;
        for (u32 slot = 0; slot < MAX_FRAMES_IN_FLIGHT; ++slot)
        {
            VkDescriptorSet set = vr.restirDescSet[slot];
            if (set == VK_NULL_HANDLE) continue;

            writes[n] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            writes[n].dstSet          = set;
            writes[n].dstBinding      = 0;
            writes[n].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[n].descriptorCount = 1;
            writes[n].pImageInfo      = &depthInfo;
            ++n;

            writes[n] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            writes[n].dstSet          = set;
            writes[n].dstBinding      = 1;
            writes[n].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[n].descriptorCount = 1;
            writes[n].pImageInfo      = &normalInfo;
            ++n;

            writes[n] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            writes[n].dstSet          = set;
            writes[n].dstBinding      = 2;
            writes[n].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[n].descriptorCount = 1;
            writes[n].pBufferInfo     = &reservoirInfo;
            ++n;

            writes[n] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            writes[n].dstSet          = set;
            writes[n].dstBinding      = 3;
            writes[n].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            writes[n].descriptorCount = 1;
            writes[n].pImageInfo      = &diInfo;
            ++n;
        }
        vkUpdateDescriptorSets(device, n, writes, 0, nullptr);
    }

    RG::ResourceHandle RtRestirSubsystem::AddPasses(RG::RenderGraph& rg,
                                                    RG::ResourceHandle sceneDepth,
                                                    RG::ResourceHandle slimNormal)
    {
        if (!m_Enabled || !m_InitialPipeline || !m_ShadePipeline) return {};

        ViewResources* preflightVr = m_Pipeline ? m_Pipeline->GetCurrentViewResources() : nullptr;
        if (!preflightVr || !preflightVr->restirDI || !preflightVr->restirReservoir.buffer) return {};
        if (m_Pipeline->GetRt().GetTlas() == VK_NULL_HANDLE) return {};

        // Build invViewProj + frameSeed once; both passes share the same push constants.
        RestirPC pc{};
        pc.invViewProj    = Math::Inverse(m_Pipeline->GetGlobal().GetCachedViewProj());
        pc.candidateCount = k_DefaultCandidateCount;
        pc.frameSeed      = static_cast<u32>(Renderer::GetFrameData()->GetRenderFrameIndex());

        // Initial pass — RIS over point lights + one visibility ray, writes per-pixel reservoir.
        // The reservoir buffer is imported ONCE here; the handle threads into the shade pass's
        // ReadBuffer so the RG sees a single VkBuffer (re-importing would alias distinct nodes).
        struct RestirInitialData {
            RG::ResourceHandle depth;
            RG::ResourceHandle normal;
            RG::BufferHandle   reservoir;
        };
        RG::BufferHandle reservoirHandle{};
        rg.AddComputePass<RestirInitialData>(
            "RestirInitial",
            RG::QueueFamily::AsyncCompute,
            [&, this](RestirInitialData& data, RG::RenderPassBuilder& builder) {
                if (sceneDepth.IsValid()) data.depth  = builder.ReadStorageImage(sceneDepth);
                if (slimNormal.IsValid()) data.normal = builder.ReadStorageImage(slimNormal);

                ViewResources* vr = m_Pipeline->GetCurrentViewResources();
                RG::BufferDesc bd{ "RestirReservoir", vr->restirReservoir.size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT };
                data.reservoir  = rg.ImportBuffer(bd, (void*)vr->restirReservoir.buffer, RG::ResourceState::Undefined);
                data.reservoir  = builder.WriteBuffer(data.reservoir);
                reservoirHandle = data.reservoir;
            },
            [this, pc](RestirInitialData&, RG::RenderPassContext& ctx) {
                VkCommandBuffer cmd = ctx.commandBuffer;
                ViewResources*  vr  = m_Pipeline->GetCurrentViewResources();
                if (!vr || vr->restirDescSet[0] == VK_NULL_HANDLE) return;

                // AS-build → AS-read barrier. dstStageMask is COMPUTE_SHADER (NOT RAY_TRACING) —
                // rayQuery executes in the compute stage; a RAY_TRACING dst here is a TDR trap.
                VkMemoryBarrier2 asBarrier{ VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
                asBarrier.srcStageMask  = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
                asBarrier.srcAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
                asBarrier.dstStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                asBarrier.dstAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR;
                VkDependencyInfo asDep{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
                asDep.memoryBarrierCount = 1;
                asDep.pMemoryBarriers    = &asBarrier;
                vkCmdPipelineBarrier2(cmd, &asDep);

                const u32 slot = static_cast<u32>(Renderer::GetFrameData()->GetRenderFrameIndex()) % MAX_FRAMES_IN_FLIGHT;
                m_InitialPipeline->Bind(cmd);
                VkDescriptorSet sets[3] = {
                    vr->globalDescriptorSet[slot],
                    vr->lightDescSet[slot],
                    vr->restirDescSet[slot],
                };
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                    m_InitialPipeline->GetLayout(), 0, 3, sets, 0, nullptr);
                vkCmdPushConstants(cmd, m_InitialPipeline->GetLayout(),
                    VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(RestirPC), &pc);

                const u32 groupX = (vr->width + 7) / 8;
                const u32 groupY = (vr->height + 7) / 8;
                vkCmdDispatch(cmd, groupX, groupY, 1);
            });

        // Shade pass — reads the resolved reservoir + depth/normal, writes demodulated DI image.
        struct RestirShadeData {
            RG::ResourceHandle depth;
            RG::ResourceHandle normal;
            RG::ResourceHandle di;
            RG::BufferHandle   reservoir;
        };
        RG::ResourceHandle diHandle{};
        rg.AddComputePass<RestirShadeData>(
            "RestirShade",
            RG::QueueFamily::AsyncCompute,
            [&, this](RestirShadeData& data, RG::RenderPassBuilder& builder) {
                if (sceneDepth.IsValid()) data.depth  = builder.ReadStorageImage(sceneDepth);
                if (slimNormal.IsValid()) data.normal = builder.ReadStorageImage(slimNormal);
                data.reservoir = builder.ReadBuffer(reservoirHandle);

                ViewResources* vr = m_Pipeline->GetCurrentViewResources();
                auto diTex = std::static_pointer_cast<VKTexture>(vr->restirDI);
                RG::TextureDesc desc;
                desc.name   = "RestirDI";
                desc.width  = diTex->GetWidth();
                desc.height = diTex->GetHeight();
                desc.format = RG::TextureFormat::RGBA16_Float;
                data.di  = rg.ImportResource(desc,
                    (void*)diTex->GetImage(), (void*)diTex->GetImageView(),
                    RG::ResourceState::Undefined);
                data.di  = builder.WriteStorageImage(data.di);
                diHandle = data.di;
            },
            [this, pc](RestirShadeData&, RG::RenderPassContext& ctx) {
                VkCommandBuffer cmd = ctx.commandBuffer;
                ViewResources*  vr  = m_Pipeline->GetCurrentViewResources();
                if (!vr || vr->restirDescSet[0] == VK_NULL_HANDLE) return;

                const u32 slot = static_cast<u32>(Renderer::GetFrameData()->GetRenderFrameIndex()) % MAX_FRAMES_IN_FLIGHT;
                m_ShadePipeline->Bind(cmd);
                VkDescriptorSet sets[3] = {
                    vr->globalDescriptorSet[slot],
                    vr->lightDescSet[slot],
                    vr->restirDescSet[slot],
                };
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                    m_ShadePipeline->GetLayout(), 0, 3, sets, 0, nullptr);
                vkCmdPushConstants(cmd, m_ShadePipeline->GetLayout(),
                    VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(RestirPC), &pc);

                const u32 groupX = (vr->width + 7) / 8;
                const u32 groupY = (vr->height + 7) / 8;
                vkCmdDispatch(cmd, groupX, groupY, 1);
            });

        return diHandle;
    }
}
