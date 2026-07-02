#pragma once

#include "luth/core/types/LuthMath.h"

namespace Luth::Component
{
    // Per-entity response to the global wind FIELD (WindSettings), for static wind-deformable meshes.
    // Absent component means the entity responds fully to the global field (all multipliers 1, no override),
    // so marking a mesh deformable needs zero per-entity setup. The deformed buffer is per-mesh-asset,
    // so these params are last-writer-wins when several entities share one deformable mesh; meaningful
    // for distinct (single-instance) hero meshes. see arch/rendering-pipeline.md
    struct Wind
    {
        bool enabled            = true;   // false -> this entity is forced to bind pose
        f32  strengthMultiplier = 1.0f;
        f32  phaseOffset        = 0.0f;   // de-syncs distinct meshes sharing the global field
        f32  gustMultiplier     = 1.0f;
        f32  detailMultiplier   = 1.0f;   // scales the detail bend + turbulence

        bool useDirectionOverride = false;
        Vec3 directionOverride    = Vec3(1.0f, 0.0f, 0.0f);
        bool overrideIsWorldSpace = true; // false -> the override is already in the mesh's object space
    };

    static_assert(sizeof(Wind) <= 64, "Wind exceeds 64-byte budget");
}
