#pragma once

#include "luth/core/types/LuthMath.h"

namespace Luth
{
    // Global procedural vertex wind: the global wind FIELD. Read each frame by the deform pass
    // (SkinningSubsystem) when filling deform.slang's push constants for static wind-deformable meshes.
    // Per-entity response (multipliers / direction override) layers on top via Component::Wind. Direction
    // is WORLD-space; the dispatch transforms it into each mesh's object space. Deformation is per-asset,
    // so instances of one mesh bend in lockstep (last-writer-wins). `enabled == false` passes strength 0
    // (bind pose). see arch/rendering-pipeline.md
    struct WindSettings
    {
        bool enabled       = true;
        Vec3 direction     = Vec3(1.0f, 0.0f, 0.0f);   // WORLD-space; normalized + transformed per instance
        f32  strength      = 0.5f;
        f32  mainBendScale = 0.25f;   // sway along the wind, scaled by vertex height (local +Y)
        f32  detailScale   = 0.05f;   // per-vertex shimmer along the normal
        f32  frequency     = 2.0f;    // wave speed (multiplies time)

        f32  gustStrength        = 0.0f;   // low-freq amplitude pulse on the main bend (0 == steady)
        f32  gustFrequency       = 0.3f;
        f32  turbulenceAmplitude = 0.0f;   // extra in-shader hash-noise octaves on the detail bend (0 == off)
        f32  turbulenceFrequency = 1.5f;
    };
}
