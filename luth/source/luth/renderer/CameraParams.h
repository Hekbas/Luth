#pragma once

#include "luth/scene/Entity.h"

#include <glm/glm.hpp>
#include <vector>

namespace Luth
{
    // Camera and editor state needed by RenderingSystem each frame.
    // Populated by App before SystemRegistry::Update<RenderingSystem>() to avoid
    // a scene-layer dependency on the editor.
    struct CameraParams
    {
        glm::mat4 view       = glm::mat4(1.0f);
        glm::mat4 projection = glm::mat4(1.0f);
        glm::vec3 position   = glm::vec3(0.0f);
        float nearZ          = 0.1f;
        float farZ           = 1000.0f;
        float iblIntensity     = 1.0f;
        float skyboxIntensity  = 1.0f;
        std::vector<Entity> selectedEntities;
    };
}
