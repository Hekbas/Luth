#pragma once

#include "luth/core/types/LuthTypes.h"

namespace Luth
{
    // Slang Phase-0 spike A/B harness (#156). `enabled` (default OFF) gates the whole harness — when off
    // nothing loads slang-compiler.dll and the RG culls the pass, so it is zero-cost. The `last*` fields
    // are written each frame by SlangParityGuard from the GPU diff readback and shown read-only in the
    // RenderPanel; the editor never reaches the subsystem directly. see spike #156
    struct SlangParitySettings
    {
        bool enabled = false;

        // Read-only verdict readout (GLSL reference vs Slang port over the covered pixels).
        f32  lastMaxAbsDiff  = 0.0f;   // max |A-B| color component this frame
        u32  lastMaxUlp      = 0;      // max per-component ULP distance (0 = bit-identical)
        u32  lastDifferingPx = 0;      // pixels with any nonzero diff
        u32  lastCoveredPx   = 0;      // pixels where either side rendered a hit
    };
}
