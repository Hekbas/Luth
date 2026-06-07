#pragma once

#include "luth/core/types/LuthTypes.h"

namespace Luth
{
    // ReSTIR DI (Bitterli 2020) runtime-tunable parameters. Read each frame by RtRestirSubsystem
    // when filling the initial/temporal/spatial push constants; `enabled` also gates restirParams.x
    // in GlobalSubsystem (pbr.frag samples the demodulated DI image only when set). Persisted
    // alongside the other render settings on the RenderingSystem. see arch/rendering-pipeline.md
    struct RestirSettings
    {
        bool enabled = true;
        u32  candidateCount = 32;        // initial RIS candidates (M)
        u32  temporalMCap = 20;          // history clamp multiplier (prev.M <= mCap * curr.M)
        f32  temporalDepthThreshold = 0.05f;
        f32  temporalNormalThreshold = 0.9f;
        u32  spatialNeighbours = 5;
        u32  spatialRadius = 16;
        f32  spatialDepthThreshold = 0.1f;
    };
}
