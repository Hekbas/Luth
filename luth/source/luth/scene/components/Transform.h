#pragma once

#include "luth/core/types/LuthMath.h"

namespace Luth::Component
{
    // Transform pair attached to every spatial entity. `Transform` holds the editable TRS plus a
    // cached LocalMatrix gated by IsDirty; `WorldTransform` is the level-parallel result that
    // TransformSystem produces each game tick and that every downstream system reads.
    struct Transform {
        Vec3 Position = { 0.0f, 0.0f, 0.0f };
        Vec3 Rotation = { 0.0f, 0.0f, 0.0f }; // Euler angles (degrees)
        Vec3 Scale    = { 1.0f, 1.0f, 1.0f };

        Mat4 LocalMatrix = Mat4(1.0f);
        bool IsDirty = true;
    };

    struct WorldTransform {
        Mat4 Matrix = Mat4(1.0f);
    };
}
