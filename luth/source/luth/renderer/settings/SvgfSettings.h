#pragma once

#include "luth/core/types/LuthTypes.h"

namespace Luth
{
    // SVGF (Schied 2017) diffuse-denoiser tunables. Read each frame by SvgfDenoiser when filling the
    // reproject/atrous push constants. `enabled` toggles denoise vs raw pass-through (the A/B compare);
    // the denoiser still owns the output image either way, so the consumer binding never changes.
    // Persisted alongside the other render settings on the RenderingSystem. see arch/rendering-pipeline.md
    struct SvgfSettings
    {
        bool enabled = true;
        u32  atrousIterations = 5;       // edge-aware wavelet levels (paper 5, Falcor 4)
        u32  historyCap = 32;            // temporal EMA history-length clamp (alpha floor = 1/cap)
        f32  phiColor = 10.0f;           // luminance edge-stop sigma — the primary per-scene tuning knob
        f32  phiNormal = 128.0f;         // normal edge-stop exponent
        f32  phiDepth = 1.0f;            // depth edge-stop scale (fwidth-normalized)
    };
}
