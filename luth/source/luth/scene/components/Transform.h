#pragma once

#include "luth/core/Math.h"

namespace Luth::Component
{
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
