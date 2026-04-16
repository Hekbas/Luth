#pragma once

#include "luth/core/LuthTypes.h"
#include "luth/renderer/rendergraph/RenderGraphSnapshot.h"
#include "luth/renderer/rendergraph/ArchivedImage.h"
#include "luth/renderer/rendergraph/FrameEventTree.h"

#include <glm/glm.hpp>
#include <string>
#include <vector>
#include <vulkan/vulkan.h>

namespace Luth::RG
{
    // Pipeline state captured at draw time (not hardcoded)
    struct CapturedPipelineState
    {
        std::string shaderName;
        u32  renderMode  = 0;       // Material::RenderMode as u32
        u32  cullMode    = 0;       // VK_CULL_MODE_* value
        VkPolygonMode polygonMode = VK_POLYGON_MODE_FILL;
        bool isSkinned   = false;
        bool depthTest   = false;
        bool depthWrite  = false;
        bool blendEnabled = false;
    };

    // Kind of GPU work captured in CapturedDrawCall
    enum class DispatchKind : u8 { Direct, IndexedIndirect, Compute };

    // One per vkCmdDrawIndexed / vkCmdDrawIndexedIndirect / vkCmdDispatch call
    struct CapturedDrawCall
    {
        u32 globalIndex     = 0;    // 0-based across entire frame
        u32 passLocalIndex  = 0;    // 0-based within its pass
        u32 passIndex       = 0;    // index into CapturedFrame::passes

        std::string passName;
        std::string meshName;       // model name + mesh index (or shader name for compute)
        std::string entityName;
        u32 entityIndex  = 0;
        u32 indexCount   = 0;

        // Snapshot of the push constants at draw time (Direct / IndexedIndirect only)
        glm::mat4 modelMatrix = glm::mat4(1.0f);
        u32 materialIndex = 0;
        u32 shadeMode     = 0;
        u32 entityID      = 0;
        u32 boneOffset    = 0;

        CapturedPipelineState pipelineState;

        // Dispatch kind + metadata
        DispatchKind kind = DispatchKind::Direct;

        // Indirect-draw metadata (kind == IndexedIndirect)
        u32          gpuObjectIndex    = 0;
        VkDeviceSize indirectOffset    = 0;
        u32          indirectDrawCount = 0;
        u32          indirectStride    = 0;

        // Compute-dispatch metadata (kind == Compute)
        u32 groupCountX = 0;
        u32 groupCountY = 0;
        u32 groupCountZ = 0;
    };

    // Aggregated info per render pass
    struct CapturedPass
    {
        std::string name;
        u32 firstDrawIndex  = 0;    // into CapturedFrame::drawCalls
        u32 drawCallCount   = 0;
        float gpuTimeMs     = -1.0f;

        CapturedPipelineState pipelineState;

        // Active render target tracking for rescue blit
        std::string activeRenderTarget;
        bool isDepthTarget = false;
    };

    // The complete captured frame data
    struct CapturedFrame
    {
        std::vector<CapturedDrawCall> drawCalls;
        std::vector<CapturedPass>     passes;
        std::vector<ResourceSnapshot> resources;
        float totalGpuTimeMs = 0.0f;
        bool  valid          = false;

        // Phase 14B — Per-pass archives + capture-time camera state.
        //
        // archivedImages owns the staging copies of tracked render targets, captured
        // post-pass during Execute by the FrameDebugger sink. passArchives is indexed
        // by RenderGraph pass index and holds the indices into archivedImages of all
        // archives produced by that pass (typically 0–4 per pass).
        //
        // captureViewProj is the camera viewProj at the moment of capture; the Frozen
        // path compares against the live viewProj to trigger auto-recapture on camera
        // movement (Phase 14C).
        //
        // ArchivedImage destruction is the OWNER's responsibility (FrameDebugger).
        // Clear() does NOT free GPU resources — call FrameDebugger::DestroyArchives
        // first.
        std::vector<ArchivedImage>     archivedImages;
        std::vector<std::vector<u32>>  passArchives;
        glm::mat4                       captureViewProj = glm::mat4(1.0f);

        // Phase 14D — Hierarchical event tree built at capture finalize from
        // passes/drawCalls + the prefix registry in FrameEventTree.cpp.
        EventNode                       rootEvent;

        // Metadata-only reset. GPU-owned archives are NOT touched; the owner
        // (FrameDebugger) must call DestroyArchives separately to free them.
        // BeginCapture orchestrates both in the right order.
        void Clear()
        {
            drawCalls.clear();
            passes.clear();
            resources.clear();
            rootEvent = EventNode{};
            captureViewProj = glm::mat4(1.0f);
            totalGpuTimeMs = 0.0f;
            valid = false;
        }
    };
}
