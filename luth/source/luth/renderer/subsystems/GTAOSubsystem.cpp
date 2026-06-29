#include "luthpch.h"
#include "luth/renderer/subsystems/GTAOSubsystem.h"
#include "luth/renderer/RenderPipeline.h"
#include "luth/renderer/Renderer.h"
#include "luth/renderer/FrameTargets.h"
#include "luth/scene/systems/RenderingSystem.h"
#include "luth/renderer/shader/ShaderLibrary.h"
#include "luth/renderer/backend/vulkan/VulkanContext.h"
#include "luth/renderer/backend/vulkan/VulkanTexture.h"
#include "luth/renderer/backend/vulkan/VulkanBuffer.h"
#include "luth/renderer/settings/GTAOSettings.h"
#include "luth/core/FrameData.h"
#include "luth/jobs/JobSystem.h"
#include "luth/memory/GPUTaggedPageAllocator.h"

#include <cmath>

namespace Luth
{
    void GTAOSubsystem::Init(RenderPipeline& pipeline)
    {
        LH_PROFILE_FUNCTION();
        m_Pipeline = &pipeline;
        VkDevice device = VulkanContext::Get().GetDevice();

        VkSamplerCreateInfo sampCI{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        sampCI.magFilter    = VK_FILTER_LINEAR;
        sampCI.minFilter    = VK_FILTER_LINEAR;
        sampCI.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampCI.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampCI.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampCI.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        vkCreateSampler(device, &sampCI, nullptr, &m_Sampler);

        // Prefilter layout: [sampler2D sceneDepth, image2D linearDepth]
        {
            VkDescriptorSetLayoutBinding bindings[2]{};
            bindings[0].binding         = 0;
            bindings[0].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            bindings[0].descriptorCount = 1;
            bindings[0].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
            bindings[1].binding         = 1;
            bindings[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            bindings[1].descriptorCount = 1;
            bindings[1].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;

            VkDescriptorSetLayoutCreateInfo layoutCI{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
            layoutCI.bindingCount = 2;
            layoutCI.pBindings    = bindings;
            vkCreateDescriptorSetLayout(device, &layoutCI, nullptr, &m_PrefilterDescLayout);

            VkPushConstantRange pcRange{ VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(i32) * 2 + sizeof(float) * 6 };

            if (auto sh = ShaderLibrary::LoadEngine("shaders/gtao_depth_prefilter.comp"))
                m_PrefilterSpv = sh->GetSpirV();
            if (m_PrefilterSpv.empty())
            {
                LH_LOG(Renderer, error, "GTAOSubsystem: failed to load gtao_depth_prefilter.comp!");
                return;
            }
            m_PrefilterPipeline = std::make_unique<VKComputePipeline>(
                m_PrefilterSpv,
                std::vector<VkDescriptorSetLayout>{ m_PrefilterDescLayout },
                std::vector<VkPushConstantRange>{ pcRange });
        }

        // Main layout: [sampler2D linearDepth, image2D rawAO, UBO]
        {
            VkDescriptorSetLayoutBinding bindings[3]{};
            bindings[0].binding         = 0;
            bindings[0].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            bindings[0].descriptorCount = 1;
            bindings[0].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
            bindings[1].binding         = 1;
            bindings[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            bindings[1].descriptorCount = 1;
            bindings[1].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
            bindings[2].binding         = 2;
            bindings[2].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            bindings[2].descriptorCount = 1;
            bindings[2].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;

            // invariant: cycling alone doesn't avoid the in-pending-cmdbuf race for
            // these per-render-stage rewrites — UAB needed (validation 03047).
            VkDescriptorBindingFlags bindingFlags[3] = {
                VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT,
                VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT,
                VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT,
            };
            VkDescriptorSetLayoutBindingFlagsCreateInfo bindingFlagsCI{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO };
            bindingFlagsCI.bindingCount  = 3;
            bindingFlagsCI.pBindingFlags = bindingFlags;

            VkDescriptorSetLayoutCreateInfo layoutCI{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
            layoutCI.pNext        = &bindingFlagsCI;
            layoutCI.flags        = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
            layoutCI.bindingCount = 3;
            layoutCI.pBindings    = bindings;
            vkCreateDescriptorSetLayout(device, &layoutCI, nullptr, &m_MainDescLayout);

            VkPushConstantRange pcRange{ VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(float) * 4 + sizeof(u32) * 4 };

            if (auto sh = ShaderLibrary::LoadEngine("shaders/gtao_main.comp"))
                m_MainSpv = sh->GetSpirV();
            if (m_MainSpv.empty())
            {
                LH_LOG(Renderer, error, "GTAOSubsystem: failed to load gtao_main.comp!");
                return;
            }
            m_MainPipeline = std::make_unique<VKComputePipeline>(
                m_MainSpv,
                std::vector<VkDescriptorSetLayout>{ m_MainDescLayout },
                std::vector<VkPushConstantRange>{ pcRange });
        }

        // Denoise layout: [sampler2D rawAO, sampler2D linDepth, image2D finalAO]
        {
            VkDescriptorSetLayoutBinding bindings[3]{};
            bindings[0].binding         = 0;
            bindings[0].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            bindings[0].descriptorCount = 1;
            bindings[0].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
            bindings[1].binding         = 1;
            bindings[1].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            bindings[1].descriptorCount = 1;
            bindings[1].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
            bindings[2].binding         = 2;
            bindings[2].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            bindings[2].descriptorCount = 1;
            bindings[2].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;

            VkDescriptorSetLayoutCreateInfo layoutCI{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
            layoutCI.bindingCount = 3;
            layoutCI.pBindings    = bindings;
            vkCreateDescriptorSetLayout(device, &layoutCI, nullptr, &m_DenoiseDescLayout);

            if (auto sh = ShaderLibrary::LoadEngine("shaders/gtao_denoise.comp"))
                m_DenoiseSpv = sh->GetSpirV();
            if (m_DenoiseSpv.empty())
            {
                LH_LOG(Renderer, error, "GTAOSubsystem: failed to load gtao_denoise.comp!");
                return;
            }
            m_DenoisePipeline = std::make_unique<VKComputePipeline>(
                m_DenoiseSpv,
                std::vector<VkDescriptorSetLayout>{ m_DenoiseDescLayout },
                std::vector<VkPushConstantRange>{});
        }
    }

    void GTAOSubsystem::Shutdown()
    {
        LH_PROFILE_FUNCTION();
        VkDevice device = VulkanContext::Get().GetDevice();
        m_PrefilterPipeline.reset();
        m_MainPipeline.reset();
        m_DenoisePipeline.reset();
        if (m_Sampler)             vkDestroySampler(device, m_Sampler, nullptr);
        if (m_PrefilterDescLayout) vkDestroyDescriptorSetLayout(device, m_PrefilterDescLayout, nullptr);
        if (m_MainDescLayout)      vkDestroyDescriptorSetLayout(device, m_MainDescLayout, nullptr);
        if (m_DenoiseDescLayout)   vkDestroyDescriptorSetLayout(device, m_DenoiseDescLayout, nullptr);
        m_Sampler             = VK_NULL_HANDLE;
        m_PrefilterDescLayout = VK_NULL_HANDLE;
        m_MainDescLayout      = VK_NULL_HANDLE;
        m_DenoiseDescLayout   = VK_NULL_HANDLE;
    }

    bool GTAOSubsystem::OnShaderReloaded(const std::string& name, const std::vector<u32>& spv)
    {
        LH_PROFILE_FUNCTION();
        auto deferComp = [](std::unique_ptr<VKComputePipeline>& p) {
            if (auto* raw = p.release(); raw)
                VulkanContext::Get().PushDeletion([raw]() { delete raw; });
        };

        if (name == "gtao_depth_prefilter.comp" && m_PrefilterDescLayout)
        {
            m_PrefilterSpv = spv;
            deferComp(m_PrefilterPipeline);
            VkPushConstantRange pc{ VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(i32) * 2 + sizeof(float) * 6 };
            m_PrefilterPipeline = std::make_unique<VKComputePipeline>(m_PrefilterSpv,
                std::vector<VkDescriptorSetLayout>{ m_PrefilterDescLayout },
                std::vector<VkPushConstantRange>{ pc });
            return true;
        }
        if (name == "gtao_main.comp" && m_MainDescLayout)
        {
            m_MainSpv = spv;
            deferComp(m_MainPipeline);
            VkPushConstantRange pc{ VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(float) * 4 + sizeof(u32) * 4 };
            m_MainPipeline = std::make_unique<VKComputePipeline>(m_MainSpv,
                std::vector<VkDescriptorSetLayout>{ m_MainDescLayout },
                std::vector<VkPushConstantRange>{ pc });
            return true;
        }
        if (name == "gtao_denoise.comp" && m_DenoiseDescLayout)
        {
            m_DenoiseSpv = spv;
            deferComp(m_DenoisePipeline);
            m_DenoisePipeline = std::make_unique<VKComputePipeline>(m_DenoiseSpv,
                std::vector<VkDescriptorSetLayout>{ m_DenoiseDescLayout },
                std::vector<VkPushConstantRange>{});
            return true;
        }
        return false;
    }

    void GTAOSubsystem::UpdateUBO()
    {
        LH_PROFILE_FUNCTION();
        ViewResources* vr = m_Pipeline->GetCurrentViewResources();
        if (!vr || vr->globalDescriptorSet[0] == VK_NULL_HANDLE) return;

        const auto& s = m_Pipeline->GetSystem().GetPostProcessSettings().gtao;
        GTAOUBO ubo{};
        ubo.intensity      = s.intensity;
        ubo.radius         = s.radius;
        ubo.falloff        = s.falloff;
        ubo.power          = s.power;
        ubo.sliceCount     = s.sliceCount;
        ubo.stepsPerSlice  = s.stepsPerSlice;
        ubo.enabled        = s.enabled  ? 1 : 0;
        ubo.visualize      = s.visualize ? 1 : 0;

        // Derive full-res from the view's half-res GTAO textures.
        const auto& lin = vr->gtaoLinearDepth;
        const u32 halfW = lin ? lin->GetWidth()  : 1u;
        const u32 halfH = lin ? lin->GetHeight() : 1u;
        const u32 fullW = halfW * 2;
        const u32 fullH = halfH * 2;
        ubo.invResolution[0]     = 1.0f / float(halfW);
        ubo.invResolution[1]     = 1.0f / float(halfH);
        ubo.invFullResolution[0] = 1.0f / float(fullW);
        ubo.invFullResolution[1] = 1.0f / float(fullH);

        // invariant: Set 0 binding 5 + GTAO main set binding 2 share the same per-frame
        // region AND the same per-frame slot. The two writes MUST stay in one batched call
        // so we don't double-allocate, and both must use the same `slot` so the next frame's
        // allocator doesn't overwrite a region the previous frame's binding still references.
        auto* jobCtx = JobSystem::GetCurrentJobContext();
        if (!jobCtx) return;
        const u32 frameAbs = static_cast<u32>(Renderer::GetFrameData()->GetRenderFrameIndex());
        const u32 slot     = frameAbs % MAX_FRAMES_IN_FLIGHT;
        jobCtx->GpuCache.CurrentTag = frameAbs;

        auto& heap   = Memory::GPUTaggedPageAllocator::Get();
        const u64 al = VulkanContext::Get().GetMinUniformBufferAlignment();
        Memory::GPUSubRegion region = heap.Allocate(jobCtx->GpuCache, sizeof(GTAOUBO), al);
        if (!region.buffer) return;

        memcpy(region.mappedPtr, &ubo, sizeof(GTAOUBO));
        heap.FlushRegion(region);

        VkDescriptorBufferInfo bi{};
        bi.buffer = region.buffer;
        bi.offset = region.offset;
        bi.range  = region.size;

        VkWriteDescriptorSet writes[2] = {};
        writes[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        writes[0].dstSet          = vr->globalDescriptorSet[slot];
        writes[0].dstBinding      = 5;
        writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[0].descriptorCount = 1;
        writes[0].pBufferInfo     = &bi;

        u32 n = 1;
        if (vr->gtaoMainDescSet[0] != VK_NULL_HANDLE)
        {
            writes[1] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            writes[1].dstSet          = vr->gtaoMainDescSet[slot];
            writes[1].dstBinding      = 2;
            writes[1].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            writes[1].descriptorCount = 1;
            writes[1].pBufferInfo     = &bi;
            ++n;
        }

        vkUpdateDescriptorSets(VulkanContext::Get().GetDevice(), n, writes, 0, nullptr);
    }

    void GTAOSubsystem::WriteView(ViewResources& vr, FrameTargets& targets)
    {
        LH_PROFILE_FUNCTION();
        if (vr.gtaoPrefilterDescSet == VK_NULL_HANDLE) return;

        VkDevice device = VulkanContext::Get().GetDevice();

        auto vkSceneDepth = std::static_pointer_cast<VKTexture>(targets.GetSceneDepth());
        auto vkLinDepth   = std::static_pointer_cast<VKTexture>(vr.gtaoLinearDepth);
        auto vkRawAO      = std::static_pointer_cast<VKTexture>(vr.gtaoRawAO);
        auto vkFinalAO    = std::static_pointer_cast<VKTexture>(vr.gtaoFinal);

        VkDescriptorImageInfo sceneDepthInfo{};
        sceneDepthInfo.sampler     = m_Sampler;
        sceneDepthInfo.imageView   = vkSceneDepth->GetImageView();
        sceneDepthInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkDescriptorImageInfo linDepthSampledInfo{};
        linDepthSampledInfo.sampler     = m_Sampler;
        linDepthSampledInfo.imageView   = vkLinDepth->GetImageView();
        linDepthSampledInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkDescriptorImageInfo linDepthStorageInfo{};
        linDepthStorageInfo.imageView   = vkLinDepth->GetImageView();
        linDepthStorageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkDescriptorImageInfo rawAOStorageInfo{};
        rawAOStorageInfo.imageView   = vkRawAO->GetImageView();
        rawAOStorageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        // Prefilter: [sceneDepth (sampler), linDepth (storage)].
        VkWriteDescriptorSet preWrites[2]{};
        preWrites[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        preWrites[0].dstSet          = vr.gtaoPrefilterDescSet;
        preWrites[0].dstBinding      = 0;
        preWrites[0].descriptorCount = 1;
        preWrites[0].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        preWrites[0].pImageInfo      = &sceneDepthInfo;
        preWrites[1] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        preWrites[1].dstSet          = vr.gtaoPrefilterDescSet;
        preWrites[1].dstBinding      = 1;
        preWrites[1].descriptorCount = 1;
        preWrites[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        preWrites[1].pImageInfo      = &linDepthStorageInfo;
        vkUpdateDescriptorSets(device, 2, preWrites, 0, nullptr);

        if (vr.gtaoMainDescSet[0] == VK_NULL_HANDLE) return;
        // Bindings 0 + 1 stable; binding 2 (UBO) rebound per render-stage in UpdateUBO.
        VkWriteDescriptorSet mainWrites[2 * MAX_FRAMES_IN_FLIGHT]{};
        for (u32 s = 0; s < MAX_FRAMES_IN_FLIGHT; ++s)
        {
            mainWrites[s * 2 + 0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            mainWrites[s * 2 + 0].dstSet          = vr.gtaoMainDescSet[s];
            mainWrites[s * 2 + 0].dstBinding      = 0;
            mainWrites[s * 2 + 0].descriptorCount = 1;
            mainWrites[s * 2 + 0].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            mainWrites[s * 2 + 0].pImageInfo      = &linDepthSampledInfo;
            mainWrites[s * 2 + 1] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            mainWrites[s * 2 + 1].dstSet          = vr.gtaoMainDescSet[s];
            mainWrites[s * 2 + 1].dstBinding      = 1;
            mainWrites[s * 2 + 1].descriptorCount = 1;
            mainWrites[s * 2 + 1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            mainWrites[s * 2 + 1].pImageInfo      = &rawAOStorageInfo;
        }
        vkUpdateDescriptorSets(device, 2 * MAX_FRAMES_IN_FLIGHT, mainWrites, 0, nullptr);

        if (vr.gtaoDenoiseDescSet == VK_NULL_HANDLE) return;
        VkDescriptorImageInfo rawAOSampledInfo{};
        rawAOSampledInfo.sampler     = m_Sampler;
        rawAOSampledInfo.imageView   = vkRawAO->GetImageView();
        rawAOSampledInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkDescriptorImageInfo finalAOStorageInfo{};
        finalAOStorageInfo.imageView   = vkFinalAO->GetImageView();
        finalAOStorageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkWriteDescriptorSet denoiseWrites[3]{};
        denoiseWrites[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        denoiseWrites[0].dstSet          = vr.gtaoDenoiseDescSet;
        denoiseWrites[0].dstBinding      = 0;
        denoiseWrites[0].descriptorCount = 1;
        denoiseWrites[0].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        denoiseWrites[0].pImageInfo      = &rawAOSampledInfo;
        denoiseWrites[1] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        denoiseWrites[1].dstSet          = vr.gtaoDenoiseDescSet;
        denoiseWrites[1].dstBinding      = 1;
        denoiseWrites[1].descriptorCount = 1;
        denoiseWrites[1].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        denoiseWrites[1].pImageInfo      = &linDepthSampledInfo;
        denoiseWrites[2] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        denoiseWrites[2].dstSet          = vr.gtaoDenoiseDescSet;
        denoiseWrites[2].dstBinding      = 2;
        denoiseWrites[2].descriptorCount = 1;
        denoiseWrites[2].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        denoiseWrites[2].pImageInfo      = &finalAOStorageInfo;
        vkUpdateDescriptorSets(device, 3, denoiseWrites, 0, nullptr);
    }

    namespace {
        struct GTAOPrefilterPC {
            IVec2 halfResSize;     // 0
            Vec2  invFullRes;      // 8
            float nearZ;           // 16
            float farZ;            // 20
            float _pad0;           // 24
            float _pad1;           // 28
        };
        static_assert(sizeof(GTAOPrefilterPC) == 32, "GTAOPrefilterPC layout mismatch");

        struct GTAOMainPC {
            Vec2  projParams;   // 0  (P[0][0], |P[1][1]|)
            float nearZ;        // 8
            float farZ;         // 12
            u32   frameIndex;   // 16
            u32   _pad0;        // 20
            u32   _pad1;        // 24
            u32   _pad2;        // 28
        };
        static_assert(sizeof(GTAOMainPC) == 32, "GTAOMainPC layout mismatch");
    }

    RG::ResourceHandle GTAOSubsystem::AddPrefilterPass(RG::RenderGraph& rg, RG::ResourceHandle sceneDepth)
    {
        LH_PROFILE_FUNCTION();
        struct GTAOPrefilterData {
            RG::ResourceHandle sceneDepth;
            RG::ResourceHandle linearDepth;
        };
        RG::ResourceHandle outputHandle;

        rg.AddComputePass<GTAOPrefilterData>("GTAODepthPrefilter", RG::QueueFamily::AsyncCompute,
            [&](GTAOPrefilterData& data, RG::RenderPassBuilder& builder)
            {
                // ReadStorageImage despite the name gives ComputeRead → SHADER_READ_ONLY layout.
                data.sceneDepth = builder.ReadStorageImage(sceneDepth);

                ViewResources* vr = m_Pipeline->GetCurrentViewResources();
                RG::TextureDesc desc;
                desc.name   = "GTAOLinearDepth";
                desc.width  = vr->gtaoLinearDepth->GetWidth();
                desc.height = vr->gtaoLinearDepth->GetHeight();
                desc.format = RG::TextureFormat::R32_Float;

                auto vkLin = std::static_pointer_cast<VKTexture>(vr->gtaoLinearDepth);
                data.linearDepth = rg.ImportResource(desc,
                    (void*)vkLin->GetImage(), (void*)vkLin->GetImageView(),
                    RG::ResourceState::Undefined);
                data.linearDepth = builder.WriteStorageImage(data.linearDepth);

                outputHandle = data.linearDepth;
            },
            [this](GTAOPrefilterData& data, RG::RenderPassContext& ctx)
            {
                VkCommandBuffer cmd = ctx.commandBuffer;
                auto& sys = m_Pipeline->GetSystem();
                ViewResources* vr = m_Pipeline->GetCurrentViewResources();

                sys.GetFrameDebugger().BeginCapturePass(ctx.passIndex, "GTAODepthPrefilter", "GTAOLinearDepth", false,
                    { "gtao_depth_prefilter", 0, 0, VK_POLYGON_MODE_FILL, false, false, false, false });

                if (!m_PrefilterPipeline || vr->gtaoPrefilterDescSet == VK_NULL_HANDLE)
                {
                    sys.GetFrameDebugger().EndCapturePass();
                    return;
                }

                m_PrefilterPipeline->Bind(cmd);
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                    m_PrefilterPipeline->GetLayout(), 0, 1, &vr->gtaoPrefilterDescSet, 0, nullptr);

                const u32 halfW = vr->gtaoLinearDepth->GetWidth();
                const u32 halfH = vr->gtaoLinearDepth->GetHeight();
                const u32 fullW = m_Pipeline->GetCurrentView()->targets->GetSceneDepth()->GetWidth();
                const u32 fullH = m_Pipeline->GetCurrentView()->targets->GetSceneDepth()->GetHeight();

                GTAOPrefilterPC pc{};
                pc.halfResSize = { (i32)halfW, (i32)halfH };
                pc.invFullRes  = { 1.0f / float(fullW), 1.0f / float(fullH) };
                pc.nearZ       = m_Pipeline->GetCurrentView()->camera.nearZ;
                pc.farZ        = m_Pipeline->GetCurrentView()->camera.farZ;
                vkCmdPushConstants(cmd, m_PrefilterPipeline->GetLayout(),
                    VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(GTAOPrefilterPC), &pc);

                const u32 groupX = (halfW + 7) / 8;
                const u32 groupY = (halfH + 7) / 8;
                vkCmdDispatch(cmd, groupX, groupY, 1);

                sys.GetFrameDebugger().CaptureComputeDispatch("GTAODepthPrefilter",
                    "gtao_depth_prefilter", groupX, groupY, 1);
                sys.GetFrameDebugger().EndCapturePass();
            });
        return outputHandle;
    }

    RG::ResourceHandle GTAOSubsystem::AddMainPass(RG::RenderGraph& rg, RG::ResourceHandle linearDepth)
    {
        LH_PROFILE_FUNCTION();
        struct GTAOMainData {
            RG::ResourceHandle linearDepth;
            RG::ResourceHandle rawAO;
        };
        RG::ResourceHandle outputHandle;

        rg.AddComputePass<GTAOMainData>("GTAOMain", RG::QueueFamily::AsyncCompute,
            [&](GTAOMainData& data, RG::RenderPassBuilder& builder)
            {
                data.linearDepth = builder.ReadStorageImage(linearDepth);

                ViewResources* vr = m_Pipeline->GetCurrentViewResources();
                RG::TextureDesc desc;
                desc.name   = "GTAORawAO";
                desc.width  = vr->gtaoRawAO->GetWidth();
                desc.height = vr->gtaoRawAO->GetHeight();
                desc.format = RG::TextureFormat::R8_Unorm;

                auto vkRaw = std::static_pointer_cast<VKTexture>(vr->gtaoRawAO);
                data.rawAO = rg.ImportResource(desc,
                    (void*)vkRaw->GetImage(), (void*)vkRaw->GetImageView(),
                    RG::ResourceState::Undefined);
                data.rawAO = builder.WriteStorageImage(data.rawAO);

                outputHandle = data.rawAO;
            },
            [this](GTAOMainData& data, RG::RenderPassContext& ctx)
            {
                VkCommandBuffer cmd = ctx.commandBuffer;
                auto& sys = m_Pipeline->GetSystem();
                ViewResources* vr = m_Pipeline->GetCurrentViewResources();

                sys.GetFrameDebugger().BeginCapturePass(ctx.passIndex, "GTAOMain", "GTAORawAO", false,
                    { "gtao_main", 0, 0, VK_POLYGON_MODE_FILL, false, false, false, false });

                if (!m_MainPipeline || vr->gtaoMainDescSet[0] == VK_NULL_HANDLE)
                {
                    sys.GetFrameDebugger().EndCapturePass();
                    return;
                }

                const u32 slot = static_cast<u32>(Renderer::GetFrameData()->GetRenderFrameIndex()) % MAX_FRAMES_IN_FLIGHT;
                m_MainPipeline->Bind(cmd);
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                    m_MainPipeline->GetLayout(), 0, 1, &vr->gtaoMainDescSet[slot], 0, nullptr);

                const u32 halfW = vr->gtaoRawAO->GetWidth();
                const u32 halfH = vr->gtaoRawAO->GetHeight();

                // Vulkan Y-flipped projection has P[1][1] < 0; pass |P[1][1]| so the shader
                // works in conventional +Y-up view space.
                const auto& P = m_Pipeline->GetCurrentView()->camera.projection;
                GTAOMainPC pc{};
                pc.projParams  = { P[0][0], std::abs(P[1][1]) };
                pc.nearZ       = m_Pipeline->GetCurrentView()->camera.nearZ;
                pc.farZ        = m_Pipeline->GetCurrentView()->camera.farZ;
                pc.frameIndex  = (u32)Renderer::GetFrameData()->GetFrameIndex();
                vkCmdPushConstants(cmd, m_MainPipeline->GetLayout(),
                    VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(GTAOMainPC), &pc);

                const u32 groupX = (halfW + 7) / 8;
                const u32 groupY = (halfH + 7) / 8;
                vkCmdDispatch(cmd, groupX, groupY, 1);

                sys.GetFrameDebugger().CaptureComputeDispatch("GTAOMain", "gtao_main", groupX, groupY, 1);
                sys.GetFrameDebugger().EndCapturePass();
            });
        return outputHandle;
    }

    RG::ResourceHandle GTAOSubsystem::AddDenoisePass(RG::RenderGraph& rg, RG::ResourceHandle rawAO, RG::ResourceHandle linearDepth)
    {
        LH_PROFILE_FUNCTION();
        struct GTAODenoiseData {
            RG::ResourceHandle rawAO;
            RG::ResourceHandle linearDepth;
            RG::ResourceHandle finalAO;
        };
        RG::ResourceHandle outputHandle;

        rg.AddComputePass<GTAODenoiseData>("GTAODenoise", RG::QueueFamily::AsyncCompute,
            [&](GTAODenoiseData& data, RG::RenderPassBuilder& builder)
            {
                data.rawAO       = builder.ReadStorageImage(rawAO);
                data.linearDepth = builder.ReadStorageImage(linearDepth);

                ViewResources* vr = m_Pipeline->GetCurrentViewResources();
                RG::TextureDesc desc;
                desc.name   = "GTAOFinal";
                desc.width  = vr->gtaoFinal->GetWidth();
                desc.height = vr->gtaoFinal->GetHeight();
                desc.format = RG::TextureFormat::R8_Unorm;

                auto vkFinal = std::static_pointer_cast<VKTexture>(vr->gtaoFinal);
                data.finalAO = rg.ImportResource(desc,
                    (void*)vkFinal->GetImage(), (void*)vkFinal->GetImageView(),
                    RG::ResourceState::Undefined);
                data.finalAO = builder.WriteStorageImage(data.finalAO);

                outputHandle = data.finalAO;
            },
            [this](GTAODenoiseData& data, RG::RenderPassContext& ctx)
            {
                VkCommandBuffer cmd = ctx.commandBuffer;
                auto& sys = m_Pipeline->GetSystem();
                ViewResources* vr = m_Pipeline->GetCurrentViewResources();

                sys.GetFrameDebugger().BeginCapturePass(ctx.passIndex, "GTAODenoise", "GTAOFinal", false,
                    { "gtao_denoise", 0, 0, VK_POLYGON_MODE_FILL, false, false, false, false });

                if (!m_DenoisePipeline || vr->gtaoDenoiseDescSet == VK_NULL_HANDLE)
                {
                    sys.GetFrameDebugger().EndCapturePass();
                    return;
                }

                m_DenoisePipeline->Bind(cmd);
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                    m_DenoisePipeline->GetLayout(), 0, 1, &vr->gtaoDenoiseDescSet, 0, nullptr);

                const u32 halfW = vr->gtaoFinal->GetWidth();
                const u32 halfH = vr->gtaoFinal->GetHeight();
                const u32 groupX = (halfW + 7) / 8;
                const u32 groupY = (halfH + 7) / 8;
                vkCmdDispatch(cmd, groupX, groupY, 1);

                sys.GetFrameDebugger().CaptureComputeDispatch("GTAODenoise", "gtao_denoise", groupX, groupY, 1);
                sys.GetFrameDebugger().EndCapturePass();
            });
        return outputHandle;
    }
}
