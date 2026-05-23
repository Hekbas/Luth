#pragma once

#include "luth/core/types/LuthMath.h"

namespace Luth::Component
{
    // Local volumetric-fog region. The injection compute pass evaluates point-in-shape per
    // voxel and modulates density + in-scatter color from this volume. Tagged-union matches
    // Collider; the active union member is selected by `type`. Multiple FogVolumes accumulate.
    struct FogVolume
    {
        enum class Type : u8
        {
            Box,
            Sphere
        };

        Type type = Type::Box;
        Vec3 localOffset{0.0f};
        Quat localRotation{1.0f, 0.0f, 0.0f, 0.0f};

        // Only the active member is read; the other is undefined per `type`.
        union
        {
            Vec3 halfExtents;
            f32  radius;
        };

        Vec3 color{1.0f};
        f32  density        = 0.1f;
        f32  falloffStart   = 0.0f;   // [0..1] of normalized distance — full contribution below
        f32  falloffEnd     = 1.0f;   // [0..1] — zero contribution at/beyond
        bool affectsAmbient = true;

        FogVolume() : halfExtents(2.0f, 2.0f, 2.0f) {}
    };

    static_assert(sizeof(FogVolume) <= 96, "FogVolume exceeds 96-byte budget");
}
