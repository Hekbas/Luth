#include "luthpch.h"
#include "luth/renderer/RenderPipeline.h"
#include "luth/scene/systems/RenderingSystem.h"
#include "luth/renderer/Renderer.h"
#include "luth/renderer/backend/vulkan/VulkanContext.h"
#include "luth/renderer/passes/CullPass.h"
#include "luth/renderer/draw/DrawCommand.h"
#include "luth/core/Math.h"

#include <string>
#include <vector>

namespace Luth
{
    RenderPipeline::RenderPipeline(RenderingSystem& system)
        : m_System(system)
    {
    }

    void RenderPipeline::ExecuteMinimal()
    {
        auto& s = m_System;
        RG::RenderGraph rg(*s.m_FrameAllocator);
        AddImGuiPass(rg, RG::ResourceHandle{}); // invalid → ImGuiPass skips the optional Read
        rg.Compile();
        Renderer::ExecuteGraph(rg, Renderer::GetFrameData()->GetFrameIndex(), nullptr);
    }

    void RenderPipeline::Execute(entt::registry& registry)
    {
        auto& s = m_System;

        RG::RenderGraph rg(*s.m_FrameAllocator);

        // Import persistent buffers into the render graph for barrier tracking.
        RG::BufferDesc objDesc {
            "ObjectSSBO",
            RenderingSystem::k_MaxGPUObjects * sizeof(GPUObjectData),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
        };
        RG::BufferDesc indDesc {
            "IndirectBuffer",
            RenderingSystem::k_IndirectRegionCount * RenderingSystem::k_IndirectRegionStride * sizeof(VkDrawIndexedIndirectCommand),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT
        };
        RG::BufferHandle hObjectBuf   = rg.ImportBuffer(objDesc, (void*)s.m_ObjectSSBO,    RG::ResourceState::Undefined);
        RG::BufferHandle hIndirectBuf = rg.ImportBuffer(indDesc, (void*)s.m_IndirectBuffer, RG::ResourceState::Undefined);

        // Frustum cull — 5 dispatches: camera region + 4 shadow cascade regions.
        // Each cascade uses its own light-space viewProj frustum so shadow casters
        // outside the camera frustum but inside the cascade still get rendered.
        {
            Frustum camFrustum = CreateFrustumFromCamera(s.m_CachedViewProj);
            AddCullComputePass(rg, hObjectBuf, hIndirectBuf,
                s.m_CullPipeline.get(), s.m_CullDescSet, camFrustum.planes, s.m_GPUObjectCount,
                /*destOffset*/ 0, "FrustumCull.Cam", &s.m_FrameDebugger);

            for (u32 i = 0; i < k_ShadowCascadeCount; ++i)
            {
                Frustum cascadeFrustum = CreateFrustumFromCamera(s.m_Cascades.lightSpaceMatrix[i]);
                const u32 destOffset = (i + 1) * RenderingSystem::k_IndirectRegionStride;
                const std::string name = "FrustumCull.C" + std::to_string(i);
                AddCullComputePass(rg, hObjectBuf, hIndirectBuf,
                    s.m_CullPipeline.get(), s.m_CullDescSet, cascadeFrustum.planes, s.m_GPUObjectCount,
                    destOffset, name.c_str(), &s.m_FrameDebugger);
            }
        }

        RG::ResourceHandle shadowHandles[k_ShadowCascadeCount];
        for (u32 i = 0; i < k_ShadowCascadeCount; ++i)
            shadowHandles[i] = AddShadowPass(rg, registry, hIndirectBuf, i);

        // Z-prepass produces SceneDepth before forward shading. The render
        // graph can schedule it in parallel with the shadow cascades.
        RG::ResourceHandle prepassDepth = AddDepthPrepass(rg, registry, hIndirectBuf);

        // GTAO chain runs every frame so the Set 0 binding-4 sampler sees
        // a valid SHADER_READ_ONLY layout (the `gtao.enabled` flag in the
        // UBO is what disables the modulation inside pbr.frag). ~0.3-1 ms
        // on a mid-range GPU at 1080p; can be gated later if a cheaper
        // bypass path is worth the complexity.
        RG::ResourceHandle gtaoLinearDepth = AddGTAODepthPrefilterPass(rg, prepassDepth);
        RG::ResourceHandle gtaoRawAO       = AddGTAOMainPass(rg, gtaoLinearDepth);
        RG::ResourceHandle gtaoFinalAO     = AddGTAODenoisePass(rg, gtaoRawAO, gtaoLinearDepth);

        auto geoOutput                 = AddGeometryPass(rg, registry, shadowHandles, hIndirectBuf, prepassDepth);
        auto maskOutput                = AddSelectionMaskPass(rg, registry);
        RG::ResourceHandle skyboxColor = AddSkyboxPass(rg, geoOutput.color, geoOutput.depth);
        RG::ResourceHandle bloomResult = AddBloomPasses(rg, skyboxColor); // bloom reads PRE-grid color so grid lines don't bloom
        RG::ResourceHandle gridColor   = s.m_GridVisible
                                         ? AddGridPass(rg, skyboxColor, geoOutput.depth)
                                         : skyboxColor;
        RG::ResourceHandle ldrOutput   = AddPostProcessPass(rg, gridColor, bloomResult);
        RG::ResourceHandle finalOutput = AddOutlinePass(rg, ldrOutput, maskOutput, geoOutput.depth);
        AddImGuiPass(rg, finalOutput);

        rg.Compile();

        // Capture render graph snapshot for Frame Debugger panel
        s.m_GraphSnapshot = CaptureSnapshot(rg);

        // Read GPU timing from completed frames and fill snapshot
        std::vector<float> gpuTimes;
        u32 nonCulledCount = 0;
        for (auto& p : s.m_GraphSnapshot.passes)
            if (!p.culled) nonCulledCount++;

        s.m_GPUTimers.ReadResults(nonCulledCount, gpuTimes);
        float totalMs = 0.0f;
        u32 timerIdx = 0;
        for (auto& p : s.m_GraphSnapshot.passes)
        {
            if (p.culled) continue;
            if (timerIdx < (u32)gpuTimes.size())
            {
                p.gpuTimeMs = gpuTimes[timerIdx];
                if (gpuTimes[timerIdx] > 0.0f) totalMs += gpuTimes[timerIdx];
            }
            timerIdx++;
        }
        s.m_GraphSnapshot.totalGpuTimeMs = totalMs;

        // --- Phase 14B — Wire archive sink for the capture frame ---
        // The sink will copy each tracked render target into a fresh ArchivedImage
        // after each pass that writes it. Keep the tracked-RT set tight to bound
        // memory (~50 MB at 1080p for the v1 set). The sink is a no-op when state
        // != CaptureRequested, so re-checking here is sufficient.
        if (s.m_FrameDebugger.state == DebuggerState::CaptureRequested)
        {
            // Phase 14D — ensure the debug sampler exists for ImGui archive previews.
            // Idempotent: returns immediately once blitPipeline is set.
            s.InitDebugBlitResources();

            // Phase 14E — invalidate the per-draw replay cache. The cache
            // is keyed by (passIdx, drawIdx) which can collide across
            // captures even though the underlying scene state has changed
            // (camera moved → recapture → same passIdx/drawIdx but new
            // GPUObjectData / IndirectBuffer contents). Without this reset,
            // re-clicking the same draw after recapture would hit the
            // stale cached preview.
            s.m_PerDrawPreviewKey = UINT64_MAX;
            // Same for the Phase 14F depth-preview blit cache — keyed by
            // (archiveIdx, layer+1); recapture rebuilds archives at the
            // same indices, so without this reset, re-selecting a depth
            // pass would skip the re-blit and show the previous frame.
            s.m_DepthPreviewKey = UINT64_MAX;

            s.m_FrameDebugger.BeginCapture(VulkanContext::Get().GetDevice(),
                                            VulkanContext::Get().GetAllocator());
            s.m_FrameDebugger.RegisterTrackedRT("SceneColor");
            s.m_FrameDebugger.RegisterTrackedRT("SceneDepth");
            // Phase 13 ShadowPass imports per-cascade resources named
            // "ShadowMap.C<i>" (one per cascade, single-layer view onto
            // the shared 4-layer array). Track each variant so the sink
            // archives them — without this, cascade nodes have no
            // primary output and the panel shows "no output preview".
            for (u32 ci = 0; ci < k_ShadowCascadeCount; ++ci)
                s.m_FrameDebugger.RegisterTrackedRT("ShadowMap.C" + std::to_string(ci));
            s.m_FrameDebugger.RegisterTrackedRT("LDROutput");
            s.m_FrameDebugger.RegisterTrackedRT("EntityID");
            s.m_FrameDebugger.RegisterTrackedRT("BloomAFinal");
            s.m_FrameDebugger.RegisterTrackedRT("GTAOLinearDepth");
            s.m_FrameDebugger.RegisterTrackedRT("GTAORawAO");
            s.m_FrameDebugger.RegisterTrackedRT("GTAOFinal");
            rg.SetArchiveSink(&s.m_FrameDebugger);
        }

        Renderer::ExecuteGraph(rg, Renderer::GetFrameData()->GetFrameIndex(), &s.m_GPUTimers);

        // --- Frame Debugger: Finalize capture and enter frozen state ---
        if (s.m_FrameDebugger.state == DebuggerState::CaptureRequested)
        {
            // Phase 14C — captured*Draws / drawLimit removed.
            // Per-draw replay (Phase 14E) re-derives draw inputs from the
            // CapturedDrawCall records + frozen indirect/object SSBOs.

            // Copy resource and timing info from the graph snapshot
            s.m_FrameDebugger.capturedFrame.resources      = s.m_GraphSnapshot.resources;
            s.m_FrameDebugger.capturedFrame.totalGpuTimeMs = s.m_GraphSnapshot.totalGpuTimeMs;

            // Copy per-pass GPU times into captured passes
            {
                u32 capturedIdx = 0;
                for (auto& ps : s.m_GraphSnapshot.passes)
                {
                    if (ps.culled) continue;
                    if (capturedIdx < s.m_FrameDebugger.capturedFrame.passes.size())
                        s.m_FrameDebugger.capturedFrame.passes[capturedIdx].gpuTimeMs = ps.gpuTimeMs;
                    capturedIdx++;
                }
            }

            // Snapshot capture-time camera viewProj for the Frozen-state
            // auto-recapture comparison (see top of Update).
            s.m_FrameDebugger.FinalizeCapture(s.m_CachedViewProj);

            // Phase 14F — stamp CSM state into the captured frame so the
            // cascade detail panel can show GPU-true values from the moment
            // of capture, even if the user later twiddles light settings.
            s.m_FrameDebugger.capturedFrame.cascadeSplitsViewZ = s.m_Cascades.splitsViewZ;
            s.m_FrameDebugger.capturedFrame.shadowBias         = s.m_ShadowParams.shadowBias;
            s.m_FrameDebugger.capturedFrame.shadowNormalBias   = s.m_ShadowParams.shadowNormalBias;
            s.m_FrameDebugger.capturedFrame.cascadeTexelSize   = s.m_Cascades.texelSize;
            for (u32 i = 0; i < k_ShadowCascadeCount; ++i)
                s.m_FrameDebugger.capturedFrame.lightSpaceMatrix[i] = s.m_Cascades.lightSpaceMatrix[i];

            s.m_FrameDebugger.capturedFrame.valid = true;
            s.m_FrameDebugger.state               = DebuggerState::Frozen;
        }
    }

    RG::RenderGraphSnapshot RenderPipeline::CaptureSnapshot(const RG::RenderGraph& rg)
    {
        auto& s = m_System;

        RG::RenderGraphSnapshot snapshot;

        // Snapshot resources
        auto& resources = const_cast<RG::RenderGraph&>(rg).GetResources();
        snapshot.resources.reserve(resources.size());
        for (auto& res : resources)
        {
            RG::ResourceSnapshot rs;
            rs.name        = res.desc.name;
            rs.width       = res.desc.width;
            rs.height      = res.desc.height;
            rs.format      = res.desc.format;
            rs.isExternal  = res.external;
            rs.isTransient = res.isTransient;
            snapshot.resources.push_back(std::move(rs));
        }

        // Snapshot passes
        auto& passes = rg.GetPasses();
        snapshot.passes.reserve(passes.size());
        for (auto& pass : passes)
        {
            RG::PassSnapshot ps;
            ps.name                = pass.name;
            ps.culled              = pass.culled;
            ps.numColorAttachments = (u32)pass.colorAttachments.size();
            ps.hasDepth            = pass.hasDepth;

            for (auto& r : pass.reads)
            {
                RG::PassSnapshotResource sr;
                sr.index = r.index;
                sr.name  = (r.index > 0 && r.index <= resources.size()) ? resources[r.index - 1].desc.name : "?";
                ps.reads.push_back(std::move(sr));
            }

            for (auto& w : pass.writes)
            {
                RG::PassSnapshotResource sw;
                sw.index = w.index;
                sw.name  = (w.index > 0 && w.index <= resources.size()) ? resources[w.index - 1].desc.name : "?";
                ps.writes.push_back(std::move(sw));
            }

            // Compute primaryOutputIndex from first color write, or depth if depth-only pass
            if (!pass.colorAttachments.empty())
            {
                u32 idx = pass.colorAttachments[0].handle.index;
                if (idx > 0 && idx <= resources.size())
                    ps.primaryOutputIndex = (int)(idx - 1);
            }
            else if (pass.hasDepth)
            {
                u32 idx = pass.depthAttachment.handle.index;
                if (idx > 0 && idx <= resources.size())
                    ps.primaryOutputIndex = (int)(idx - 1);
            }

            snapshot.passes.push_back(std::move(ps));
        }

        // Compute geometry stats from the current DrawList (built before pass dispatch)
        u32 totalDraws = (u32)(s.m_DrawList.opaque.size() + s.m_DrawList.cutout.size() + s.m_DrawList.transparent.size());
        u32 totalIndices = 0;
        auto sumIndices = [&](const std::vector<DrawCommand>& draws) {
            for (auto& dc : draws)
            {
                if (!dc.model) continue;
                auto mesh = dc.model->GetMesh(dc.meshIndex);
                if (mesh && mesh->GetIndexBuffer())
                    totalIndices += mesh->GetIndexBuffer()->GetCount();
            }
        };
        sumIndices(s.m_DrawList.opaque);
        sumIndices(s.m_DrawList.cutout);
        sumIndices(s.m_DrawList.transparent);

        // Enrich per-pass pipeline state (known at RenderingSystem level, not RenderGraph)
        for (auto& ps : snapshot.passes)
        {
            if (ps.culled) continue;

            if (ps.name == "ShadowPass")
            {
                ps.depthTest = true; ps.depthWrite = true;
                ps.blendEnabled = false;
                ps.cullMode = VK_CULL_MODE_FRONT_BIT;
                ps.shaderName = "shadowDepth";
                ps.drawCalls = totalDraws;
                ps.indices = totalIndices;
            }
            else if (ps.name == "GeometryPass")
            {
                ps.depthTest = true; ps.depthWrite = true;
                ps.blendEnabled = false;
                ps.cullMode = VK_CULL_MODE_BACK_BIT;
                ps.shaderName = "pbr";
                ps.drawCalls = totalDraws;
                ps.indices = totalIndices;
            }
            else if (ps.name == "SkyboxPass")
            {
                ps.depthTest = true; ps.depthWrite = false;
                ps.blendEnabled = false;
                ps.cullMode = VK_CULL_MODE_BACK_BIT;
                ps.shaderName = "skybox";
                ps.drawCalls = 1; ps.indices = 0;
            }
            else if (ps.name == "BloomExtract")
            {
                ps.depthTest = false; ps.depthWrite = false;
                ps.blendEnabled = false;
                ps.cullMode = VK_CULL_MODE_NONE;
                ps.shaderName = "bloomExtract";
                ps.drawCalls = 1; ps.indices = 0;
            }
            else if (ps.name == "BloomBlurH" || ps.name == "BloomBlurV")
            {
                ps.depthTest = false; ps.depthWrite = false;
                ps.blendEnabled = false;
                ps.cullMode = VK_CULL_MODE_NONE;
                ps.shaderName = "bloomBlur";
                ps.drawCalls = 1; ps.indices = 0;
            }
            else if (ps.name == "PostProcess")
            {
                ps.depthTest = false; ps.depthWrite = false;
                ps.blendEnabled = false;
                ps.cullMode = VK_CULL_MODE_NONE;
                ps.shaderName = "postprocess";
                ps.drawCalls = 1; ps.indices = 0;
            }
            else if (ps.name == "ImGuiPass")
            {
                ps.depthTest = false; ps.depthWrite = false;
                ps.blendEnabled = true;
                ps.cullMode = VK_CULL_MODE_NONE;
                ps.shaderName = "imgui";
                ps.drawCalls = 0; ps.indices = 0; // ImGui manages its own draws
            }
        }

        return snapshot;
    }
}
