#pragma once

#include "luth/renderer/material/Material.h"

namespace Luth
{
    // Shared per-instance predicate: is this mesh instance an emissive AREA LIGHT? The
    // EmissiveLightGatherer (which produces the triangle lights) and TlasBuilder (which sets the
    // geometry-table emitter bit) MUST agree, or ReSTIR DI/GI double-count or lose emitter energy.
    // The two GLOBAL gates (feature enabled, ReSTIR DI enabled) are applied by the callers; this is the
    // per-instance part only. Skinned/deformable are excluded: their CPU bind-pose triangles would cast
    // light + shadows from the wrong place (the deformed positions live GPU-only). see arch/rendering-pipeline.md
    inline bool IsEmissiveLightMaterial(const Material& mat, bool isSkinned, bool isDeformable)
    {
        if (isSkinned || isDeformable) return false;
        const Vec3 le = mat.GetEmissiveColor() * mat.GetEmissiveStrength();
        return (le.x + le.y + le.z) > 0.0f;
    }
}
