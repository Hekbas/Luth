#include "luthpch.h"
#include "luth/renderer/subsystems/SlangSpikeSubsystem.h"
#include "luth/renderer/subsystems/RtSubsystem.h"
#include "luth/renderer/RenderPipeline.h"
#include "luth/renderer/Renderer.h"
#include "luth/renderer/shader/ShaderLibrary.h"
#include "luth/renderer/shader/SlangCompiler.h"
#include "luth/renderer/material/MaterialSystem.h"
#include "luth/renderer/settings/SlangSpikeSettings.h"
#include "luth/renderer/backend/vulkan/VulkanContext.h"
#include "luth/renderer/backend/vulkan/VulkanTexture.h"
#include "luth/scene/systems/RenderingSystem.h"
#include "luth/resources/FileSystem.h"
#include "luth/core/FrameData.h"
#include "luth/core/diagnostics/Log.h"
#include "luth/core/types/LuthMath.h"

#include <vma/vk_mem_alloc.h>
#include <bit>
#include <cstring>

namespace Luth
{
    namespace
    {
        // Matches the push constant in BOTH slang_spike_gi.comp and slang_spike_gi.slang.
        struct SpikePC {
            Mat4 invViewProj;   // 64 B — column-major, reconstructs the primary camera ray
            u64  geomTableBDA;  //  8 B — geometry-table BDA, same slot as restir_gi_initial
        };
        static_assert(sizeof(SpikePC) == 72, "SpikePC must match slang_spike_gi.{comp,slang} push_constant");

        void TransitionToGeneral(VkImage img)
        {
            VulkanContext::Get().ImmediateSubmit([&](VkCommandBuffer cmd) {
                VkImageMemoryBarrier2 b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
                b.srcStageMask        = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
                b.srcAccessMask       = 0;
                b.dstStageMask        = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                b.dstAccessMask       = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
                b.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
                b.newLayout           = VK_IMAGE_LAYOUT_GENERAL;
                b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;   // CONCURRENT sharing — never zero-init (rg-depth-handoff)
                b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                b.image               = img;
                b.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
                VkDependencyInfo dep{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
                dep.imageMemoryBarrierCount = 1;
                dep.pImageMemoryBarriers    = &b;
                vkCmdPipelineBarrier2(cmd, &dep);
            });
        }
    }

    bool SlangSpikeSubsystem::IsEnabled() const
    {
        return m_Pipeline && m_Pipeline->GetSystem().GetSlangSpikeSettings().enabled;
    }

    void SlangSpikeSubsystem::Init(RenderPipeline& pipeline)
    {
        m_Pipeline = &pipeline;   // lazy: the Slang compile + pipeline build defer to the first enable
    }

    bool SlangSpikeSubsystem::EnsureInitialized()
    {
        if (m_Initialized) return m_InitOk;
        m_Initialized = true;
        VkDevice device = VulkanContext::Get().GetDevice();

        // GLSL reference + diff reducer through the existing libshaderc path.
        if (auto sh = ShaderLibrary::LoadEngine("shaders/slang_spike_gi.comp"))   m_GlslSpv = sh->GetSpirV();
        if (auto sh = ShaderLibrary::LoadEngine("shaders/slang_spike_diff.comp"))  m_DiffSpv = sh->GetSpirV();

        // Slang port through the in-process compiler — the first call creates the global session, so
        // this is the runtime check that slang-compiler.dll + siblings load (spike item 1).
        const fs::path slangPath = FileSystem::EngineAssetsPath("shaders") / "slang_spike_gi.slang";
        m_SlangSpv = SlangCompiler::Compile(slangPath, "main");

        if (m_GlslSpv.empty() || m_DiffSpv.empty())
        {
            LH_CORE_ERROR("SlangSpike: missing slang_spike_gi.comp / slang_spike_diff.comp SPIR-V");
            return false;
        }
        if (m_SlangSpv.empty())
        {
            LH_CORE_ERROR("SlangSpike: in-process Slang compile FAILED — A/B disabled (NO-GO signal for #156)");
            return false;
        }
        LH_CORE_INFO("SlangSpike: Slang compiled slang_spike_gi.slang -> {} SPIR-V words", m_SlangSpv.size());

        // Set 2 (pass-local) — single binding: the per-variant output storage image.
        VkDescriptorSetLayoutBinding outBind{};
        outBind.binding         = 0;
        outBind.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        outBind.descriptorCount = 1;
        outBind.stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
        VkDescriptorSetLayoutCreateInfo outCI{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        outCI.bindingCount = 1;
        outCI.pBindings    = &outBind;
        vkCreateDescriptorSetLayout(device, &outCI, nullptr, &m_OutSetLayout);

        // Diff Set 0 — b0/b1 input storage images (GLSL + Slang outputs), b2 verdict SSBO.
        VkDescriptorSetLayoutBinding diffBinds[3]{};
        diffBinds[0].binding = 0; diffBinds[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;  diffBinds[0].descriptorCount = 1; diffBinds[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        diffBinds[1].binding = 1; diffBinds[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;  diffBinds[1].descriptorCount = 1; diffBinds[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        diffBinds[2].binding = 2; diffBinds[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; diffBinds[2].descriptorCount = 1; diffBinds[2].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        VkDescriptorSetLayoutCreateInfo diffCI{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        diffCI.bindingCount = 3;
        diffCI.pBindings    = diffBinds;
        vkCreateDescriptorSetLayout(device, &diffCI, nullptr, &m_DiffSetLayout);

        // Identical 5-set layout to RtRestirGiSubsystem's initial pass — global+TLAS / lights / pass-local
        // output / Material SSBO / bindless textures. The GLSL and Slang pipelines share it exactly, so
        // the compiler is the only variable.
        const std::vector<VkDescriptorSetLayout> spikeLayouts = {
            m_Pipeline->GetGlobal().GetSetLayout(),
            m_Pipeline->GetLighting().GetSetLayout(),
            m_OutSetLayout,
            MaterialSystem::GetDescriptorSetLayout(),
            VulkanContext::Get().GetBindlessSet().GetLayout(),
        };
        VkPushConstantRange pcRange{ VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(SpikePC) };

        m_GlslPipeline  = std::make_unique<VKComputePipeline>(m_GlslSpv,  spikeLayouts, std::vector<VkPushConstantRange>{ pcRange });
        // vkCreateComputePipelines accepting this is the runtime proof the Slang SPIR-V is GPU-valid (items 2/3).
        m_SlangPipeline = std::make_unique<VKComputePipeline>(m_SlangSpv, spikeLayouts, std::vector<VkPushConstantRange>{ pcRange });

        const std::vector<VkDescriptorSetLayout> diffLayouts = { m_DiffSetLayout };
        m_DiffPipeline  = std::make_unique<VKComputePipeline>(m_DiffSpv, diffLayouts);

        VkDescriptorPoolSize poolSizes[2] = {
            { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,  4 },   // 2 spike sets (1 each) + diff (2)
            { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 },   // diff verdict block
        };
        VkDescriptorPoolCreateInfo poolCI{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        poolCI.maxSets       = 3;
        poolCI.poolSizeCount = 2;
        poolCI.pPoolSizes    = poolSizes;
        vkCreateDescriptorPool(device, &poolCI, nullptr, &m_Pool);

        VkDescriptorSetAllocateInfo ai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        ai.descriptorPool     = m_Pool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts        = &m_OutSetLayout;
        vkAllocateDescriptorSets(device, &ai, &m_GlslSet);
        vkAllocateDescriptorSets(device, &ai, &m_SlangSet);
        ai.pSetLayouts        = &m_DiffSetLayout;
        vkAllocateDescriptorSets(device, &ai, &m_DiffSet);

        m_InitOk = true;
        RunLinkSpecCheck();   // #156 item 6 — exercise link-time spec across 2 entry-point stages
        return m_InitOk;
    }

    // Compose the compute + fragment entries of slang_spike_link.slang into ONE linked program and emit
    // each (CompileModuleEntries). PASS = both stages produce SPIR-V that loads as a VkShaderModule; the
    // validation layer (Debug) vets the modules. Offline spirv-val corroborates in the writeup. slang#9578.
    void SlangSpikeSubsystem::RunLinkSpecCheck()
    {
        const fs::path linkPath = FileSystem::EngineAssetsPath("shaders") / "slang_spike_link.slang";
        const std::vector<SlangCompiler::EntryReq> entries = {
            { "csMain", ShaderStage::Compute },
            { "fsMain", ShaderStage::Fragment },
        };
        auto blobs = SlangCompiler::CompileModuleEntries(linkPath, entries);

        bool ok = (blobs.size() == 2) && !blobs[0].empty() && !blobs[1].empty();
        if (ok)
        {
            VkDevice device = VulkanContext::Get().GetDevice();
            for (const auto& spv : blobs)
            {
                VkShaderModuleCreateInfo mci{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
                mci.codeSize = spv.size() * sizeof(u32);
                mci.pCode    = spv.data();
                VkShaderModule mod = VK_NULL_HANDLE;
                if (vkCreateShaderModule(device, &mci, nullptr, &mod) != VK_SUCCESS) { ok = false; break; }
                vkDestroyShaderModule(device, mod, nullptr);
            }
        }
        LH_CORE_INFO("SlangSpike link-spec (#9578): compute+fragment from one linked generic -> {}",
                     ok ? "PASS" : "FAIL");
    }

    void SlangSpikeSubsystem::DestroyResources()
    {
        m_ImgGlsl.reset();
        m_ImgSlang.reset();
        if (m_DiffBuf) VulkanAllocator::FreeBuffer(m_DiffBuf, m_DiffAlloc);
        m_DiffBuf   = VK_NULL_HANDLE;
        m_DiffAlloc = nullptr;
        m_Width = m_Height = 0;
    }

    void SlangSpikeSubsystem::Shutdown()
    {
        VkDevice device = VulkanContext::Get().GetDevice();
        m_GlslPipeline.reset();
        m_SlangPipeline.reset();
        m_DiffPipeline.reset();
        DestroyResources();
        if (m_Pool)          vkDestroyDescriptorPool(device, m_Pool, nullptr);
        if (m_OutSetLayout)  vkDestroyDescriptorSetLayout(device, m_OutSetLayout, nullptr);
        if (m_DiffSetLayout) vkDestroyDescriptorSetLayout(device, m_DiffSetLayout, nullptr);
        m_Pool = VK_NULL_HANDLE;
        m_OutSetLayout = m_DiffSetLayout = VK_NULL_HANDLE;
        m_GlslSet = m_SlangSet = m_DiffSet = VK_NULL_HANDLE;
        m_GlslSpv.clear(); m_SlangSpv.clear(); m_DiffSpv.clear();
        m_InitOk      = false;
        m_Initialized = false;
        m_Pipeline    = nullptr;
    }

    bool SlangSpikeSubsystem::OnShaderReloaded(const std::string& name, const std::vector<u32>& spv)
    {
        if (!m_InitOk) return false;
        VkPushConstantRange pcRange{ VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(SpikePC) };
        auto defer = [](std::unique_ptr<VKComputePipeline>& p) {
            if (auto* raw = p.release()) VulkanContext::Get().PushDeletion([raw]() { delete raw; });
        };
        // Only the GLSL-side shaders ride ShaderWatcher (.comp); the .slang variant is Init-only (the
        // watcher has no .slang dispatch yet — Phase 1). Rebuilding the GLSL reference still A/B-validates.
        if (name == "slang_spike_gi.comp")
        {
            const std::vector<VkDescriptorSetLayout> spikeLayouts = {
                m_Pipeline->GetGlobal().GetSetLayout(),
                m_Pipeline->GetLighting().GetSetLayout(),
                m_OutSetLayout,
                MaterialSystem::GetDescriptorSetLayout(),
                VulkanContext::Get().GetBindlessSet().GetLayout(),
            };
            m_GlslSpv = spv;
            defer(m_GlslPipeline);
            m_GlslPipeline = std::make_unique<VKComputePipeline>(m_GlslSpv, spikeLayouts, std::vector<VkPushConstantRange>{ pcRange });
            return true;
        }
        if (name == "slang_spike_diff.comp")
        {
            m_DiffSpv = spv;
            defer(m_DiffPipeline);
            m_DiffPipeline = std::make_unique<VKComputePipeline>(m_DiffSpv, std::vector<VkDescriptorSetLayout>{ m_DiffSetLayout });
            return true;
        }
        return false;
    }

    void SlangSpikeSubsystem::EnsureResources(u32 width, u32 height)
    {
        if (m_ImgGlsl && m_Width == width && m_Height == height) return;
        if (m_ImgGlsl) { vkDeviceWaitIdle(VulkanContext::Get().GetDevice()); DestroyResources(); }   // resize — rare, stall ok

        VkDevice device = VulkanContext::Get().GetDevice();
        m_ImgGlsl  = std::make_shared<VKTexture>(width, height, TextureFormat::RGBA32F, 1, 0, 1, VK_IMAGE_USAGE_STORAGE_BIT);
        m_ImgSlang = std::make_shared<VKTexture>(width, height, TextureFormat::RGBA32F, 1, 0, 1, VK_IMAGE_USAGE_STORAGE_BIT);
        TransitionToGeneral(m_ImgGlsl->GetImage());
        TransitionToGeneral(m_ImgSlang->GetImage());

        VkBufferCreateInfo bufCI{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bufCI.size  = 4 * sizeof(u32);   // { maxAbsDiffBits, differingPixels, maxUlpDiff, coveredPixels }
        bufCI.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        m_DiffAlloc = VulkanAllocator::AllocateBuffer(bufCI, VMA_MEMORY_USAGE_GPU_TO_CPU, m_DiffBuf);

        VkDescriptorImageInfo glslImg{};  glslImg.imageView  = m_ImgGlsl->GetImageView();  glslImg.imageLayout  = VK_IMAGE_LAYOUT_GENERAL;
        VkDescriptorImageInfo slangImg{}; slangImg.imageView = m_ImgSlang->GetImageView(); slangImg.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        VkDescriptorBufferInfo diffBuf{ m_DiffBuf, 0, VK_WHOLE_SIZE };

        VkWriteDescriptorSet w[5]{};
        w[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET }; w[0].dstSet = m_GlslSet;  w[0].dstBinding = 0; w[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;  w[0].descriptorCount = 1; w[0].pImageInfo  = &glslImg;
        w[1] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET }; w[1].dstSet = m_SlangSet; w[1].dstBinding = 0; w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;  w[1].descriptorCount = 1; w[1].pImageInfo  = &slangImg;
        w[2] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET }; w[2].dstSet = m_DiffSet;  w[2].dstBinding = 0; w[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;  w[2].descriptorCount = 1; w[2].pImageInfo  = &glslImg;
        w[3] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET }; w[3].dstSet = m_DiffSet;  w[3].dstBinding = 1; w[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;  w[3].descriptorCount = 1; w[3].pImageInfo  = &slangImg;
        w[4] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET }; w[4].dstSet = m_DiffSet;  w[4].dstBinding = 2; w[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[4].descriptorCount = 1; w[4].pBufferInfo = &diffBuf;
        vkUpdateDescriptorSets(device, 5, w, 0, nullptr);

        m_Width = width;
        m_Height = height;
    }

    void SlangSpikeSubsystem::ReadbackDiff()
    {
        if (!m_DiffBuf) return;
        u32 vals[4] = { 0, 0, 0, 0 };
        void* mapped = VulkanAllocator::Map(m_DiffAlloc);
        std::memcpy(vals, mapped, sizeof(vals));
        VulkanAllocator::Unmap(m_DiffAlloc);

        SlangSpikeSettings& s = m_Pipeline->GetSystem().GetSlangSpikeSettings();
        s.lastMaxAbsDiff  = std::bit_cast<f32>(vals[0]);
        s.lastDifferingPx = vals[1];
        s.lastMaxUlp      = vals[2];
        s.lastCoveredPx   = vals[3];

        if ((m_LogThrottle++ & 63u) == 0u)
            LH_CORE_INFO("SlangSpike A/B: covered={} differing={} maxUlp={} maxAbsDiff={:.6f}",
                         s.lastCoveredPx, s.lastDifferingPx, s.lastMaxUlp, s.lastMaxAbsDiff);
    }

    void SlangSpikeSubsystem::AddPass(RG::RenderGraph& rg)
    {
        if (!IsEnabled() || !EnsureInitialized()) return;   // first enable lazily loads + builds Slang
        if (!m_GlslPipeline || !m_SlangPipeline || !m_DiffPipeline) return;

        ViewResources* vr = m_Pipeline->GetCurrentViewResources();
        if (!vr || vr->width == 0 || vr->height == 0) return;
        if (m_Pipeline->GetRt().GetTlas() == VK_NULL_HANDLE) return;

        const u64 frameAbs = Renderer::GetFrameData()->GetRenderFrameIndex();
        if (m_LastRunFrame == frameAbs) return;   // scene-global A/B — first view only
        m_LastRunFrame = frameAbs;

        EnsureResources(vr->width, vr->height);
        ReadbackDiff();   // previous frame's verdict (1 frame stale — fine for a static-scene diagnostic)

        SpikePC pc{};
        pc.invViewProj  = Math::Inverse(m_Pipeline->GetGlobal().GetCachedViewProj());
        pc.geomTableBDA = m_Pipeline->GetRt().GetGeometryTableBDA();

        struct SpikeData {};
        rg.AddComputePass<SpikeData>(
            "SlangSpikeAB",
            RG::QueueFamily::AsyncCompute,
            [](SpikeData&, RG::RenderPassBuilder& builder) {
                builder.SetHasSideEffect();   // engine-owned outputs (images + host SSBO) — keep past culling
            },
            [this, pc](SpikeData&, RG::RenderPassContext& ctx) {
                VkCommandBuffer cmd = ctx.commandBuffer;
                ViewResources*  vr  = m_Pipeline->GetCurrentViewResources();
                if (!vr) return;
                const u32 slot = static_cast<u32>(Renderer::GetFrameData()->GetRenderFrameIndex()) % MAX_FRAMES_IN_FLIGHT;

                // AS-build → AS-read (COMPUTE, not RAY_TRACING — rayQuery runs in the compute stage).
                VkMemoryBarrier2 asB{ VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
                asB.srcStageMask  = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
                asB.srcAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
                asB.dstStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                asB.dstAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR;
                VkDependencyInfo asDep{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
                asDep.memoryBarrierCount = 1;
                asDep.pMemoryBarriers    = &asB;
                vkCmdPipelineBarrier2(cmd, &asDep);

                vkCmdFillBuffer(cmd, m_DiffBuf, 0, VK_WHOLE_SIZE, 0);
                VkMemoryBarrier2 clrB{ VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
                clrB.srcStageMask  = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
                clrB.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
                clrB.dstStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                clrB.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
                VkDependencyInfo clrDep{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
                clrDep.memoryBarrierCount = 1;
                clrDep.pMemoryBarriers    = &clrB;
                vkCmdPipelineBarrier2(cmd, &clrDep);

                const u32 groupX = (vr->width + 7) / 8;
                const u32 groupY = (vr->height + 7) / 8;

                VkDescriptorSet glslSets[5] = {
                    vr->globalDescriptorSet[slot], vr->lightDescSet[slot], m_GlslSet,
                    MaterialSystem::GetDescriptorSet(slot), VulkanContext::Get().GetBindlessSet().GetSet(),
                };
                m_GlslPipeline->Bind(cmd);
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_GlslPipeline->GetLayout(), 0, 5, glslSets, 0, nullptr);
                vkCmdPushConstants(cmd, m_GlslPipeline->GetLayout(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(SpikePC), &pc);
                vkCmdDispatch(cmd, groupX, groupY, 1);

                VkDescriptorSet slangSets[5] = {
                    vr->globalDescriptorSet[slot], vr->lightDescSet[slot], m_SlangSet,
                    MaterialSystem::GetDescriptorSet(slot), VulkanContext::Get().GetBindlessSet().GetSet(),
                };
                m_SlangPipeline->Bind(cmd);
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_SlangPipeline->GetLayout(), 0, 5, slangSets, 0, nullptr);
                vkCmdPushConstants(cmd, m_SlangPipeline->GetLayout(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(SpikePC), &pc);
                vkCmdDispatch(cmd, groupX, groupY, 1);

                // Both image writes → diff reads (GENERAL stays; memory + exec dependency only).
                VkMemoryBarrier2 imgB{ VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
                imgB.srcStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                imgB.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
                imgB.dstStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                imgB.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
                VkDependencyInfo imgDep{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
                imgDep.memoryBarrierCount = 1;
                imgDep.pMemoryBarriers    = &imgB;
                vkCmdPipelineBarrier2(cmd, &imgDep);

                VkDescriptorSet diffSets[1] = { m_DiffSet };
                m_DiffPipeline->Bind(cmd);
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_DiffPipeline->GetLayout(), 0, 1, diffSets, 0, nullptr);
                vkCmdDispatch(cmd, groupX, groupY, 1);
            });
    }
}
