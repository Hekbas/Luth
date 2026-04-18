#pragma once

#include "luth/scene/Entity.h"

#include <vector>

namespace Luth
{
    // Camera and editor state needed by RenderingSystem each frame.
    // Populated by App before SystemRegistry::Update<RenderingSystem>() to avoid
    // a scene-layer dependency on the editor.
    struct CameraParams
    {
        Mat4 view       = Mat4(1.0f);
        Mat4 projection = Mat4(1.0f);
        Vec3 position   = Vec3(0.0f);
        float nearZ          = 0.1f;
        float farZ           = 1000.0f;
        float iblIntensity     = 1.0f;
        float skyboxIntensity  = 1.0f;
        std::vector<Entity> selectedEntities;
    };
}
