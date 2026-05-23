#pragma once

#include "luth/core/types/LuthMath.h"

namespace Luth
{
    // CPU authoring values for global volumetric parameters. Mirrored each frame into the Global UBO
    // (distance fog + height fog + multi-scatter scalar) so the composite + inject shaders can read
    // them without a separate binding. Persisted alongside PostProcessSettings on the project.
    struct VolumetricSettings
    {
        // Distance fog — exponential extinction with camera-to-fragment distance.
        bool distanceFogEnabled    = true;
        f32  distanceFogDensity    = 0.01f;
        f32  distanceFogStart      = 0.0f;
        f32  distanceFogMaxOpacity = 0.85f;
        Vec3 distanceFogColor      = Vec3(0.5f, 0.6f, 0.7f);

        // Height fog — exponential extinction with world-space Y vs reference height.
        bool heightFogEnabled   = false;
        f32  heightFogDensity   = 0.05f;
        f32  heightFogRefHeight = 0.0f;
        f32  heightFogFalloff   = 0.1f;
        Vec3 heightFogColor     = Vec3(0.4f, 0.5f, 0.6f);

        // Hillaire 2nd-order multi-scatter coefficient. 0 = disabled.
        f32  multiScatterIntensity = 0.0f;
    };
}
