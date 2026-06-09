#pragma once

#include "luth/core/types/LuthTypes.h"

namespace Luth
{
    // Path-traced reference mode (rt-renderer C.5) runtime knobs. Read each frame by PathTraceSubsystem
    // into the megakernel push constants. RenderMode::PathTrace is the on/off gate (not a field here);
    // the progressive accumulation resets whenever any of these change. see arch/rendering-pipeline.md
    struct PathTraceSettings
    {
        u32  samplesPerFrame = 1;     // paths/pixel dispatched per frame (1..4 — high counts risk a TDR)
        u32  maxBounces      = 8;     // path-length cap (1..16)
        u32  rrStartDepth    = 3;     // Russian roulette begins after this bounce depth
        f32  fireflyClamp    = 20.0f; // per-bounce radiance-luminance clamp (higher = closer to true ground truth)
        bool accumulate      = true;  // progressive accumulation; off = single-frame (noisy) preview
    };
}
