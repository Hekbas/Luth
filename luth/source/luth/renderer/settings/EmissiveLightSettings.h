#pragma once

#include "luth/core/types/LuthTypes.h"

namespace Luth
{
    // Emissive-as-area-lights runtime toggle. When enabled (and ReSTIR DI is on), emissive-material
    // triangles are gathered into the LightSSBO and sampled as area lights by ReSTIR DI / GI / reflections.
    // Rides with DI: DI off -> no emissive area lighting (emitters keep self-glow + the GI on-hit seed).
    // see arch/rendering-pipeline.md
    struct EmissiveLightSettings
    {
        bool enabled     = true;
        f32  minPowerLum = 1e-4f;   // drop triangles whose luminous power (2pi*area*Lum(avgLe)) is below this
    };
}
