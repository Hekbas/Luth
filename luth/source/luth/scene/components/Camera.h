#pragma once

#include "luth/core/Math.h"

namespace Luth::Component
{
    struct Camera {
        enum class ProjectionType { Perspective = 0, Orthographic = 1 };

        ProjectionType Projection = ProjectionType::Perspective;

        // Perspective properties
        float VerticalFOV = 45.0f;
        float NearClip = 0.01f;
        float FarClip = 1000.0f;

        // Orthographic properties
        float OrthographicSize = 10.0f;
        float OrthographicNear = -1.0f;
        float OrthographicFar = 1.0f;

        float AspectRatio = 16.0f / 9.0f;

        Mat4 ViewMatrix;
        Mat4 ProjectionMatrix;

        bool IsDirty = true;

        Camera() = default;
        Camera(const Camera&) = default;
    };
}
