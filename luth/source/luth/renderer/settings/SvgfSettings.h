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
        // Temporal accumulation (reproject pass).
        f32  alphaColor = 0.2f;          // steady-state EMA alpha (color); per-pixel floor = 1/historyCap
        f32  alphaMoments = 0.2f;        // steady-state EMA alpha (luminance moments)
        u32  historyCap = 32;            // max temporal history length (alpha floor = 1/cap)
        f32  depthThreshold = 0.05f;     // relative linear-depth disocclusion tolerance
        f32  normalThreshold = 0.9f;     // min dot(prevN, currN) to accept reprojected history
        f32  antiFireflySigma = 8.0f;    // 3x3 mean + k*sigma clamp on the incoming sample; 0 = off
        f32  confidenceScale = 1.0f;     // reservoir-confidence history-cap shortening; 0 = off (motion-variant channels only)
        // Variance-guided a-trous wavelet (lands with the spatial filter).
        u32  atrousIterations = 5;       // edge-aware wavelet levels (paper 5, Falcor 4)
        f32  phiColor = 10.0f;           // luminance edge-stop sigma; the primary per-scene tuning knob
        f32  phiNormal = 128.0f;         // normal edge-stop exponent
        f32  phiDepth = 1.0f;            // depth edge-stop scale
        f32  phiRough = 0.15f;           // roughness edge-stop scale; spec channels only, 0 = off
    };
}
