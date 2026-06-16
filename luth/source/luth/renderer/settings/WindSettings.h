#pragma once

#include "luth/core/types/LuthMath.h"

namespace Luth
{
    // Global procedural vertex wind (D3). Read each frame by the deform pass (SkinningSubsystem) when
    // filling deform.comp's push constants for static wind-deformable meshes. Object-space + per-asset,
    // so all instances of a deformable mesh bend in lockstep — per-entity authoring + gusts/masks are
    // D4. `enabled == false` passes strength 0 (deform compute writes bind pose). see arch/rendering-pipeline.md
    struct WindSettings
    {
        bool enabled       = true;
        Vec3 direction     = Vec3(1.0f, 0.0f, 0.0f);   // normalized at the use site
        f32  strength      = 0.5f;
        f32  mainBendScale = 0.25f;   // sway along the wind, scaled by vertex height (local +Y)
        f32  detailScale   = 0.05f;   // per-vertex shimmer along the normal
        f32  frequency     = 2.0f;    // wave speed (multiplies time)
    };
}
