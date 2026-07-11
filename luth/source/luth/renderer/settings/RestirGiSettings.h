#pragma once

#include "luth/core/types/LuthTypes.h"

namespace Luth
{
    // ReSTIR GI (Ouyang 2021) runtime-tunable parameters. Read each frame by RtRestirGiSubsystem when
    // filling the initial/temporal/spatial push constants; `enabled` also gates restirParams.y in
    // GlobalSubsystem (pbr.frag adds the demodulated indirect-diffuse image only when set). Persisted
    // alongside the other render settings on the RenderingSystem. see arch/rendering-pipeline.md
    struct RestirGiSettings
    {
        bool enabled = true;
        bool halfResolution = false;        // trace + denoise GI at half-res + bilateral upscale (perf)
        f32  secondaryAlbedo = 0.5f;        // S0 scaffold constant secondary albedo (S3: real material)
        f32  maxIndirect = 10.0f;           // firefly clamp on secondary-radiance luminance
        u32  temporalMCap = 8;              // history clamp multiplier (prev.M <= mCap * curr.M)
        u32  maxReservoirAge = 30;          // discard temporal samples older than this many frames
        f32  temporalDepthThreshold = 0.05f;
        f32  temporalNormalThreshold = 0.9f;
        u32  spatialNeighbours = 2;
        u32  spatialRadius = 32;
        f32  spatialDepthThreshold = 0.1f;
        f32  spatialNormalThreshold = 0.6f;
        f32  boilingStrength = 0.2f;        // spatial-tail boiling filter on luminance(radiance)*W; 0 = off
    };
}
