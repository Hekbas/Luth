#include "luthpch.h"
#include "luth/renderer/subsystems/PathTraceSubsystem.h"
#include "luth/renderer/subsystems/RtSubsystem.h"
#include "luth/renderer/RenderPipeline.h"
#include "luth/renderer/Renderer.h"
#include "luth/renderer/shader/ShaderLibrary.h"
#include "luth/scene/systems/RenderingSystem.h"
#include "luth/renderer/backend/vulkan/VulkanContext.h"
#include "luth/renderer/backend/vulkan/VulkanTexture.h"
#include "luth/renderer/material/MaterialSystem.h"
#include "luth/core/FrameData.h"
#include "luth/core/types/LuthMath.h"

namespace Luth
{
    namespace {
        // Megakernel push constants. S0 uses invViewProj + frameSeed only; S1+ fill the rest. The
        // pipeline reserves a fixed 128 B range (k_PtPCSize) so growing this struct never touches the
        // pipeline layout (geomTableBDA + bounce params land in the reserved tail).
        struct PtPC {
            Mat4  invViewProj;
            u32   frameSeed;
            u32   sampleCount;       // accumulated paths so far (running-mean weight, S1)
            u32   samplesPerFrame;
            u32   maxBounces;
            u32   rrStartDepth;
            u32   reset;             // 1 → discard the accumulator this frame (S1)
            f32   fireflyClamp;
            f32   pad0;
        };
        static_assert(sizeof(PtPC) == 96, "PtPC must match path_trace.comp push_constant prefix");
        constexpr u32 k_PtPCSize = 128;  // fixed range — S1+ grows PtPC into the reserved tail, no layout change
    }

    bool PathTraceSubsystem::IsEnabled() const
    {
        return m_Pipeline && m_Pipeline->GetSystem().GetRenderMode() == RenderMode::PathTrace;
    }

    void PathTraceSubsystem::Init(RenderPipeline& pipeline)
    {
        m_Pipeline = &pipeline;
        VkDevice device = VulkanContext::Get().GetDevice();

        // Set 2 (pass-local) — b0 fp32 accumulator (in-place running mean), b1 fp16 display color.
        // Both STORAGE_IMAGE, kept GENERAL, stable per-view (no per-frame swap → no UAB).
        VkDescriptorSetLayoutBinding bindings[2]{};
        bindings[0].binding         = 0;
        bindings[0].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        bindings[0].descriptorCount = 1;
        bindings[0].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
        bindings[1].binding         = 1;
        bindings[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        bindings[1].descriptorCount = 1;
        bindings[1].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;

        VkDescriptorSetLayoutCreateInfo layoutCI{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        layoutCI.bindingCount = 2;
        layoutCI.pBindings    = bindings;
        vkCreateDescriptorSetLayout(device, &layoutCI, nullptr, &m_SetLayout);

        if (auto sh = ShaderLibrary::LoadEngine("shaders/path_trace.comp"))
            m_Spv = sh->GetSpirV();
        if (m_Spv.empty())
        {
            LH_CORE_ERROR("PathTraceSubsystem: failed to load path_trace.comp SPIR-V");
            return;
        }

        // Sets: 0 = global (UBO b0 + TLAS b6), 1 = light SSBO, 2 = pass-local, 3 = Material SSBO,
        // 4 = bindless textures. Matches restir_gi_initial's layout so S1's secondary-hit material
        // fetch drops in without a layout change (S0's stub references only Sets 0 + 2).
        const std::vector<VkDescriptorSetLayout> layouts = {
            m_Pipeline->GetGlobal().GetSetLayout(),
            m_Pipeline->GetLighting().GetSetLayout(),
            m_SetLayout,
            MaterialSystem::GetDescriptorSetLayout(),
            VulkanContext::Get().GetBindlessSet().GetLayout(),
        };
        VkPushConstantRange pcRange{ VK_SHADER_STAGE_COMPUTE_BIT, 0, k_PtPCSize };
        m_PtPipeline = std::make_unique<VKComputePipeline>(
            m_Spv, layouts, std::vector<VkPushConstantRange>{ pcRange });
    }

    void PathTraceSubsystem::Shutdown()
    {
        VkDevice device = VulkanContext::Get().GetDevice();
        m_PtPipeline.reset();
        if (m_SetLayout) vkDestroyDescriptorSetLayout(device, m_SetLayout, nullptr);
        m_SetLayout = VK_NULL_HANDLE;
        m_Spv.clear();
        m_Pipeline = nullptr;
    }

    bool PathTraceSubsystem::OnShaderReloaded(const std::string& name, const std::vector<u32>& spv)
    {
        if (m_SetLayout == VK_NULL_HANDLE || !m_Pipeline) return false;
        if (name != "path_trace.comp") return false;

        const std::vector<VkDescriptorSetLayout> layouts = {
            m_Pipeline->GetGlobal().GetSetLayout(),
            m_Pipeline->GetLighting().GetSetLayout(),
            m_SetLayout,
            MaterialSystem::GetDescriptorSetLayout(),
            VulkanContext::Get().GetBindlessSet().GetLayout(),
        };
        VkPushConstantRange pcRange{ VK_SHADER_STAGE_COMPUTE_BIT, 0, k_PtPCSize };
        m_Spv = spv;
        if (auto* raw = m_PtPipeline.release(); raw)
            VulkanContext::Get().PushDeletion([raw]() { delete raw; });
        m_PtPipeline = std::make_unique<VKComputePipeline>(
            m_Spv, layouts, std::vector<VkPushConstantRange>{ pcRange });
        return true;
    }

    void PathTraceSubsystem::WriteView(ViewResources& vr)
    {
        if (vr.ptDescSet == VK_NULL_HANDLE || !vr.ptAccum || !vr.ptColor) return;

        VkDescriptorImageInfo accumInfo{};
        accumInfo.imageView   = std::static_pointer_cast<VKTexture>(vr.ptAccum)->GetImageView();
        accumInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkDescriptorImageInfo colorInfo{};
        colorInfo.imageView   = std::static_pointer_cast<VKTexture>(vr.ptColor)->GetImageView();
        colorInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkWriteDescriptorSet writes[2]{};
        writes[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        writes[0].dstSet          = vr.ptDescSet;
        writes[0].dstBinding      = 0;
        writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[0].descriptorCount = 1;
        writes[0].pImageInfo      = &accumInfo;
        writes[1] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        writes[1].dstSet          = vr.ptDescSet;
        writes[1].dstBinding      = 1;
        writes[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[1].descriptorCount = 1;
        writes[1].pImageInfo      = &colorInfo;
        vkUpdateDescriptorSets(VulkanContext::Get().GetDevice(), 2, writes, 0, nullptr);
    }

    RG::ResourceHandle PathTraceSubsystem::AddPasses(RG::RenderGraph& rg)
    {
        if (!IsEnabled() || !m_PtPipeline) return {};
        ViewResources* preflightVr = m_Pipeline ? m_Pipeline->GetCurrentViewResources() : nullptr;
        if (!preflightVr || !preflightVr->ptColor || preflightVr->ptDescSet == VK_NULL_HANDLE) return {};

        const u32 frameAbs = static_cast<u32>(Renderer::GetFrameData()->GetRenderFrameIndex());
        const PathTraceSettings& s = m_Pipeline->GetSystem().GetPathTraceSettings();

        PtPC pc{};
        pc.invViewProj     = Math::Inverse(m_Pipeline->GetGlobal().GetCachedViewProj());
        pc.frameSeed       = frameAbs;
        pc.sampleCount     = preflightVr->ptSampleCount;
        pc.samplesPerFrame = s.samplesPerFrame;
        pc.maxBounces      = s.maxBounces;
        pc.rrStartDepth    = s.rrStartDepth;
        pc.reset           = 0;   // S1 wires the accumulation reset
        pc.fireflyClamp    = s.fireflyClamp;

        struct PtData { RG::ResourceHandle color; };
        RG::ResourceHandle colorHandle{};
        rg.AddComputePass<PtData>(
            "PathTrace",
            RG::QueueFamily::AsyncCompute,
            [&, this](PtData& data, RG::RenderPassBuilder& builder) {
                ViewResources* vr = m_Pipeline->GetCurrentViewResources();
                auto colTex = std::static_pointer_cast<VKTexture>(vr->ptColor);
                RG::TextureDesc desc;
                desc.name   = "PathTraceColor";
                desc.width  = colTex->GetWidth();
                desc.height = colTex->GetHeight();
                desc.format = RG::TextureFormat::RGBA16_Float;
                // Fully overwritten each frame → Undefined import (the restirGiDI pattern). The post
                // chain Reads this handle, so the RG transitions it GENERAL → SHADER_READ_ONLY for the
                // bloom/composite sample (cross-queue, semaphore-gated by the per-view 3-submit topology).
                data.color  = rg.ImportResource(desc,
                    (void*)colTex->GetImage(), (void*)colTex->GetImageView(),
                    RG::ResourceState::Undefined);
                data.color  = builder.WriteStorageImage(data.color);
                colorHandle = data.color;
            },
            [this, pc](PtData&, RG::RenderPassContext& ctx) {
                VkCommandBuffer cmd = ctx.commandBuffer;
                ViewResources*  vr  = m_Pipeline->GetCurrentViewResources();
                if (!vr || vr->ptDescSet == VK_NULL_HANDLE) return;

                const u32 slot = static_cast<u32>(Renderer::GetFrameData()->GetRenderFrameIndex()) % MAX_FRAMES_IN_FLIGHT;
                m_PtPipeline->Bind(cmd);
                VkDescriptorSet sets[5] = {
                    vr->globalDescriptorSet[slot],
                    vr->lightDescSet[slot],
                    vr->ptDescSet,
                    MaterialSystem::GetDescriptorSet(slot),
                    VulkanContext::Get().GetBindlessSet().GetSet(),
                };
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                    m_PtPipeline->GetLayout(), 0, 5, sets, 0, nullptr);
                vkCmdPushConstants(cmd, m_PtPipeline->GetLayout(),
                    VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(PtPC), &pc);

                const u32 groupX = (vr->width + 7) / 8;
                const u32 groupY = (vr->height + 7) / 8;
                vkCmdDispatch(cmd, groupX, groupY, 1);
            });

        return colorHandle;
    }
}
