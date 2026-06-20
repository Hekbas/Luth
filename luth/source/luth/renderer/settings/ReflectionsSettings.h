#pragma once

#include "luth/core/types/LuthTypes.h"

namespace Luth
{
    // RT specular reflections (rt-renderer D.1) runtime knobs. Read each frame by ReflectionsSubsystem
    // into the trace push constants + by GlobalSubsystem into reflParams (pbr.frag composite gate).
    // The reflection lobe is GGX-VNDF importance-sampled from the slim G-buffer; rough surfaces fall
    // back to the prefiltered-env IBL (RT contributes little there and the denoiser can't keep up).
    // see arch/rendering-pipeline.md
    struct ReflectionsSettings
    {
        bool enabled            = true;
        bool halfResolution     = false;    // trace + denoise reflections half-res + bilateral upscale (perf)
        // Roughness composite band: full RT below Start, smoothstep to prefiltered-env IBL, pure IBL
        // above End (the trace skips those pixels and writes the env fallback). Bhaal's damp floor /
        // altar metal sit below Start.
        f32  roughnessFadeStart = 0.45f;
        f32  roughnessFadeEnd   = 0.65f;
        f32  maxRayDistance     = 1000.0f;  // reflection-ray tMax
        f32  fireflyClamp       = 8.0f;     // per-ray radiance-luminance clamp (bounds 1-spp variance)
        bool denoise            = true;     // specular denoiser (D.1 S3); off = raw 1-spp for A/B
    };
}
