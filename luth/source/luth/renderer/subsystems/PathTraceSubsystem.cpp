#include "luthpch.h"
#include "luth/renderer/subsystems/PathTraceSubsystem.h"
#include "luth/renderer/subsystems/RtSubsystem.h"
#include "luth/renderer/RenderPipeline.h"
#include "luth/renderer/Renderer.h"
#include "luth/renderer/shader/ShaderLibrary.h"
#include "luth/scene/systems/RenderingSystem.h"
#include "luth/scene/systems/SystemRegistry.h"
#include "luth/scene/systems/LightingSystem.h"
#include "luth/renderer/backend/vulkan/VulkanContext.h"
#include "luth/renderer/backend/vulkan/VulkanTexture.h"
#include "luth/renderer/material/MaterialSystem.h"
#include "luth/core/RenderSnapshot.h"
#include "luth/core/FrameData.h"
#include "luth/core/types/LuthMath.h"

#include <algorithm>
#include <cstring>

namespace Luth
{
    namespace {
        // Megakernel push constants. S0 uses invViewProj + frameSeed only; S1+ fill the rest. The
        // pipeline reserves a fixed 128 B range (k_PtPCSize) so growing this struct never touches the
        // pipeline layout (geomTableBDA + bounce params land in the reserved tail).
        struct PtPC {
            Mat4  invViewProj;
            u32   frameSeed;
            u32   sampleCount;       // paths accumulated BEFORE this frame (running-mean weight)
            u32   samplesPerFrame;
            u32   maxBounces;
            u32   rrStartDepth;
            u32   reset;             // 1 → ignore the accumulator this frame
            f32   fireflyClamp;
            f32   pad0;
            u32   envReady;          // 1 → IBL prefiltered env bound (else ray-miss returns black)
            u32   pad1;
            u64   geomTableBDA;      // secondary-hit material fetch (paired with the bound TLAS)
        };
        static_assert(sizeof(PtPC) == 112, "PtPC must match path_trace.comp push_constant");
        constexpr u32 k_PtPCSize = 128;  // fixed range — leaves tail headroom (S2/S3 add no new fields)
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

    u64 PathTraceSubsystem::ComputeResetHash() const
    {
        auto mix  = [](u64 h, u64 v) { return (h ^ v) * 0x100000001b3ull; };
        auto mixF = [&](u64 h, f32 f) { u32 b; std::memcpy(&b, &f, 4); return mix(h, static_cast<u64>(b)); };

        u64 h = 0xcbf29ce484222325ull;          // FNV-1a basis
        h = mix(h, static_cast<u64>(m_ResetSalt));

        // Camera VP — covers position / rotation / FOV. Copy to a local (accessor may return by value).
        Mat4 vp = m_Pipeline->GetGlobal().GetCachedViewProj();
        const f32* vpF = &vp[0][0];
        for (int i = 0; i < 16; ++i) h = mixF(h, vpF[i]);

        // Scene instances — full world matrix + material + mesh + entity. Hashes more than the TLAS
        // dirty-skip (translation only) so a pure rotation also restarts: never stale, only slower.
        const RenderSnapshot& snap = m_Pipeline->GetSystem().GetActiveSnapshot();
        for (const auto& m : snap.meshes)
        {
            const f32* wm = &m.worldMatrix[0][0];
            for (int i = 0; i < 16; ++i) h = mixF(h, wm[i]);
            h = mix(h, m.materialUUID.GetHalf0());
            h = mix(h, m.materialUUID.GetHalf1());
            h = mix(h, (static_cast<u64>(m.meshIndex) << 32) ^ static_cast<u64>(m.entity));
        }

        // Lights — directional + every point light (edits here don't rebuild the TLAS, so they need
        // their own signal). LightingSystem holds the final gathered set by graph-build time.
        if (auto* ls = SystemRegistry::GetSystem<LightingSystem>())
        {
            const GatheredLights& L = ls->GetLights();
            h = mixF(h, L.dirLight.direction.x); h = mixF(h, L.dirLight.direction.y); h = mixF(h, L.dirLight.direction.z);
            h = mixF(h, L.dirLight.intensity);
            h = mixF(h, L.dirLight.color.x); h = mixF(h, L.dirLight.color.y); h = mixF(h, L.dirLight.color.z);
            for (const auto& p : L.points)
            {
                h = mixF(h, p.position.x); h = mixF(h, p.position.y); h = mixF(h, p.position.z); h = mixF(h, p.range);
                h = mixF(h, p.color.x);    h = mixF(h, p.color.y);    h = mixF(h, p.color.z);    h = mixF(h, p.intensity);
            }
        }

        // Environment exposure — env-on-miss scales by skyboxIntensity, so an edit changes the image.
        if (const RenderView* view = m_Pipeline->GetCurrentView())
        {
            h = mixF(h, view->camera.skyboxIntensity);
            h = mixF(h, view->camera.iblIntensity);
        }

        // Settings — changing the integrator (bounces / samples / RR / clamp) restarts the reference.
        const PathTraceSettings& s = m_Pipeline->GetSystem().GetPathTraceSettings();
        h = mix(h, static_cast<u64>(s.samplesPerFrame));
        h = mix(h, static_cast<u64>(s.maxBounces));
        h = mix(h, static_cast<u64>(s.rrStartDepth));
        h = mixF(h, s.fireflyClamp);
        h = mix(h, s.accumulate ? 1ull : 0ull);
        return h;
    }

    RG::ResourceHandle PathTraceSubsystem::AddPasses(RG::RenderGraph& rg)
    {
        if (!IsEnabled() || !m_PtPipeline) return {};
        ViewResources* vr = m_Pipeline ? m_Pipeline->GetCurrentViewResources() : nullptr;
        if (!vr || !vr->ptAccum || !vr->ptColor || vr->ptDescSet == VK_NULL_HANDLE) return {};
        if (m_Pipeline->GetRt().GetTlas() == VK_NULL_HANDLE) return {};

        const u32 frameAbs = static_cast<u32>(Renderer::GetFrameData()->GetRenderFrameIndex());
        const PathTraceSettings& s = m_Pipeline->GetSystem().GetPathTraceSettings();

        // Reset detection (CPU, once per view per frame): a mismatch in the radiance-affecting state
        // restarts the running mean. accumulate=off forces a single-frame (noisy) preview every frame.
        const bool accumulate = s.accumulate;
        const u64  sceneHash   = ComputeResetHash();
        const bool reset = (vr->ptResetHash != sceneHash) || !accumulate;
        vr->ptResetHash = sceneHash;
        if (reset) vr->ptSampleCount = 0u;

        PtPC pc{};
        pc.invViewProj     = Math::Inverse(m_Pipeline->GetGlobal().GetCachedViewProj());
        pc.frameSeed       = frameAbs;
        pc.sampleCount     = vr->ptSampleCount;
        pc.samplesPerFrame = std::max(s.samplesPerFrame, 1u);
        pc.maxBounces      = std::max(s.maxBounces, 1u);   // ≥1 → at least the primary hit
        pc.rrStartDepth    = s.rrStartDepth;
        pc.reset           = reset ? 1u : 0u;
        pc.fireflyClamp    = s.fireflyClamp;
        pc.envReady        = m_Pipeline->GetLighting().IsIBLReady() ? 1u : 0u;
        // Geometry-table BDA read at preflight, paired with the same m_LastResult that binds Set 0 b6 —
        // so the table can't desync from the bound TLAS. Zero before the first real build (all rays miss).
        pc.geomTableBDA    = m_Pipeline->GetRt().GetGeometryTableBDA();

        struct PtData { RG::ResourceHandle accum; RG::ResourceHandle color; };
        RG::ResourceHandle colorHandle{};
        rg.AddComputePass<PtData>(
            "PathTrace",
            RG::QueueFamily::AsyncCompute,
            [&, this](PtData& data, RG::RenderPassBuilder& builder) {
                ViewResources* v = m_Pipeline->GetCurrentViewResources();

                // fp32 accumulator — imported in its LEFT state (ComputeWrite, where last frame's
                // WriteStorageImage left it) so the RG emits the cross-frame RAW barrier that makes
                // last frame's write visible to this frame's imageLoad. Read+Write in one pass = an
                // in-place RMW (no intra-pass barrier; the GIReservoir temporal uses the same shape).
                auto accTex = std::static_pointer_cast<VKTexture>(v->ptAccum);
                RG::TextureDesc accDesc;
                accDesc.name   = "PathTraceAccum";
                accDesc.width  = accTex->GetWidth();
                accDesc.height = accTex->GetHeight();
                accDesc.format = RG::TextureFormat::RGBA16_Float;   // informational for an import; real image is RGBA32F
                data.accum = rg.ImportResource(accDesc,
                    (void*)accTex->GetImage(), (void*)accTex->GetImageView(),
                    RG::ResourceState::ComputeWrite);
                data.accum = builder.ReadStorageImageGeneral(data.accum);
                data.accum = builder.WriteStorageImage(data.accum);

                // fp16 display copy — fully overwritten each frame → Undefined import (restirGiDI pattern).
                // The post chain Reads colorHandle, so the RG transitions it GENERAL → SHADER_READ_ONLY for
                // the bloom/composite sample (cross-queue, semaphore-gated by the per-view 3-submit topology).
                auto colTex = std::static_pointer_cast<VKTexture>(v->ptColor);
                RG::TextureDesc colDesc;
                colDesc.name   = "PathTraceColor";
                colDesc.width  = colTex->GetWidth();
                colDesc.height = colTex->GetHeight();
                colDesc.format = RG::TextureFormat::RGBA16_Float;
                data.color  = rg.ImportResource(colDesc,
                    (void*)colTex->GetImage(), (void*)colTex->GetImageView(),
                    RG::ResourceState::Undefined);
                data.color  = builder.WriteStorageImage(data.color);
                colorHandle = data.color;
            },
            [this, pc](PtData&, RG::RenderPassContext& ctx) {
                VkCommandBuffer cmd = ctx.commandBuffer;
                ViewResources*  v   = m_Pipeline->GetCurrentViewResources();
                if (!v || v->ptDescSet == VK_NULL_HANDLE) return;

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
                m_PtPipeline->Bind(cmd);
                VkDescriptorSet sets[5] = {
                    v->globalDescriptorSet[slot],
                    v->lightDescSet[slot],
                    v->ptDescSet,
                    MaterialSystem::GetDescriptorSet(slot),
                    VulkanContext::Get().GetBindlessSet().GetSet(),
                };
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                    m_PtPipeline->GetLayout(), 0, 5, sets, 0, nullptr);
                vkCmdPushConstants(cmd, m_PtPipeline->GetLayout(),
                    VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(PtPC), &pc);

                const u32 groupX = (v->width + 7) / 8;
                const u32 groupY = (v->height + 7) / 8;
                vkCmdDispatch(cmd, groupX, groupY, 1);
            });

        // Advance the accumulated-sample count for next frame (clamped — keeps the running-mean weight
        // 1/(n+1) from collapsing to fp32 epsilon over a very long parked-camera session).
        if (accumulate)
            vr->ptSampleCount = std::min(vr->ptSampleCount + pc.samplesPerFrame, 65536u);
        m_LastSampleCount = vr->ptSampleCount;   // editor convergence readout

        return colorHandle;
    }
}
