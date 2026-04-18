#pragma once

#include "luth/core/Math.h"

namespace Luth::Component
{
    struct Transform {
        glm::vec3 Position = { 0.0f, 0.0f, 0.0f };
        glm::vec3 Rotation = { 0.0f, 0.0f, 0.0f }; // Euler angles (degrees)
        glm::vec3 Scale    = { 1.0f, 1.0f, 1.0f };

        glm::mat4 LocalMatrix = glm::mat4(1.0f);
        bool IsDirty = true;
    };

    struct WorldTransform {
        glm::mat4 Matrix = glm::mat4(1.0f);
    };
}
