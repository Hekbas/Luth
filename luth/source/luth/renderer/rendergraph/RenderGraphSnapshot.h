#pragma once

#include "luth/core/LuthTypes.h"
#include "luth/renderer/rendergraph/RenderGraphResources.h"

#include <string>
#include <vector>

namespace Luth::RG
{
    struct PassSnapshotResource
    {
        u32   index = 0;    // ResourceHandle.index (1-based)
        std::string name;
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
    };
}
