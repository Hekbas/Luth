#pragma once

#include "luth/core/types/LuthTypes.h"
#include "luth/renderer/draw/DrawCommand.h"

#include <vector>

namespace Luth
{
    // Three RenderMode-sorted DrawCommand buckets + per-frame summary stats.
    // Populated by DrawListBuilder once per frame; consumed by Geometry /
    // Shadow / DepthPrepass / Selection passes. Vectors are reused across
    // frames (Clear() just resets sizes) to avoid per-frame heap churn.
    struct DrawList
    {
        std::vector<DrawCommand> opaque;
        std::vector<DrawCommand> cutout;
        std::vector<DrawCommand> transparent;

        u32 visibleTriCount = 0;

        void Clear()
        {
            opaque.clear();
            cutout.clear();
            transparent.clear();
            visibleTriCount = 0;
        }
    };
}
