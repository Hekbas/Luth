#pragma once

#include "luth/core/types/LuthTypes.h"
#include "luth/renderer/rendergraph/RenderGraphResources.h"

#include <string>
#include <vector>

namespace Luth::RG
{
    // Read-only view of last frame's compiled graph. RenderGraph::Execute populates it; the
    // Frame Debugger panel and the ProfilerPanel pass-time chart consume it. Decoupled from the
    // live RenderGraph so the editor can read across frame boundaries without grabbing any lock.
    struct PassSnapshotResource
    {
        u32   index = 0;    // ResourceHandle.index (1-based)
        std::string name;
    };

    // Per-pass GPU pipeline statistics, graphics passes only (async-compute queues can't run graphics
    // stat queries). valid=false when stats capture is off or the pass recorded none.
    struct GpuPipelineStats
    {
        u64  inputVertices   = 0;
        u64  inputPrimitives = 0;
        u64  vsInvocations   = 0;
        u64  clipInvocations = 0;
        u64  clipPrimitives  = 0;
        u64  fsInvocations   = 0;   // overdraw proxy
        bool valid = false;
    };

    // One barrier the RG solver emitted, captured from the compiled graph for the inspector. redundant =
    // before==after (no layout transition; e.g. a WAW/RAW memory barrier, not necessarily wasteful).
    struct BarrierRecord
    {
        std::string resource;
        std::string before;
        std::string after;
        std::string reason;        // BarrierReason (Waw / Final / ...)
        u32  passIndex = 0;
        bool isImage   = true;
        bool isPost    = false;    // post-pass barrier (e.g. final/Present) vs pre-pass
        bool redundant = false;
    };

    struct PassSnapshot
    {
        std::string name;
        bool culled = false;

        std::vector<PassSnapshotResource> reads;
        std::vector<PassSnapshotResource> writes;

        u32  numColorAttachments = 0;
        bool hasDepth = false;

        float gpuTimeMs = -1.0f;  // -1 = no data yet

        // Pipeline state
        bool  depthTest    = false;
        bool  depthWrite   = false;
        bool  blendEnabled = false;
        u32   cullMode     = 0;         // VK_CULL_MODE_* value
        std::string shaderName;

        // Geometry stats
        u32 drawCalls = 0;
        u32 indices   = 0;

        // GPU pipeline statistics (graphics passes only; valid when stats capture was on)
        GpuPipelineStats stats;

        // Solved-barrier counts (populated when barrier capture is on)
        u32 numImageBarriers  = 0;
        u32 numBufferBarriers = 0;

        // Primary output resource index (first color write) for auto-preview
        int primaryOutputIndex = -1;    // Index into resources[] (0-based)
    };

    struct ResourceSnapshot
    {
        std::string   name;
        u32           width  = 0;
        u32           height = 0;
        TextureFormat format = TextureFormat::RGBA8_Unorm;
        bool          isExternal  = false;
        bool          isTransient = true;
    };

    struct RenderGraphSnapshot
    {
        std::vector<PassSnapshot>     passes;
        std::vector<ResourceSnapshot> resources;
        float totalGpuTimeMs = 0.0f;
        GpuPipelineStats totalStats;   // summed over graphics passes (valid when stats capture is on)

        // Solved barriers (populated when barrier capture is on; empty otherwise)
        std::vector<BarrierRecord> barriers;
        u32 numImageBarriers     = 0;
        u32 numBufferBarriers    = 0;
        u32 numRedundantBarriers = 0;
    };
}
