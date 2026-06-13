#pragma once

#include "luth/core/types/LuthTypes.h"

namespace Luth
{
    // Bindless-SPIR-V regression guard. The GATE is a deterministic scan of the compiled slang_spike_gi.slang
    // SPIR-V (run when the guard initialises + on .slang hot-reload): the bindless rayQuery path must keep its
    // NonUniform decorations and the capabilities that make them valid, which slang#10525-class regressions
    // drop or misplace. The pixel A/B below is a default-OFF visual DIAGNOSTIC only — per-frame, view- and
    // TAA-jitter-dependent (a ~1-ULP camera-ray difference flips silhouette hits), so it is informational,
    // never a pass/fail signal. `enabled` gates the runtime A/B dispatch (needs a TLAS). RenderPanel reads
    // these read-only; the editor never reaches the subsystem directly.
    struct SlangParitySettings
    {
        bool enabled = false;   // runtime A/B diagnostic dispatch (NOT the gate)

        // Deterministic SPIR-V verdict — the gate.
        bool spirvChecked    = false;  // the scan has run at least once (guard initialised / .slang reloaded)
        bool spirvPass       = true;   // capsOk && nonUniformCount > 0
        bool capsOk          = true;   // RuntimeDescriptorArray + PhysicalStorageBuffer + RayQuery + ShaderNonUniform
        u32  nonUniformCount = 0;      // OpDecorate NonUniform count on the bindless accesses (slang#10525 zeroes it)

        // Visual diagnostic readout (GLSL vs Slang pixel diff over the covered pixels) — informational only.
        f32  lastMaxAbsDiff  = 0.0f;   // max |A-B| colour component this frame
        u32  lastMaxUlp      = 0;      // max per-component ULP distance (noisy near zero)
        u32  lastDifferingPx = 0;      // pixels with any nonzero diff
        u32  lastCoveredPx   = 0;      // pixels where either side rendered a hit
    };
}
