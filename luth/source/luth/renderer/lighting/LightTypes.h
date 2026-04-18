#pragma once

#include "luth/core/LuthTypes.h"

#include <glm/glm.hpp>

namespace Luth
{
    // Number of shadow cascades for directional-light CSM (Phase 13)
    inline constexpr u32 k_ShadowCascadeCount = 4;
    inline constexpr u32 k_ShadowResolution   = 2048;

    // ---- Light data structs (mirrored in pbr.frag Set 3) ----

    struct DirectionalLightData {
        glm::vec3 direction;   // 12
        float     intensity;   // 4
        glm::vec3 color;       // 12
        float     _pad;        // 4
    };  // 32 bytes

    struct PointLightData {
        glm::vec3 position;    // 12
        float     range;       // 4
        glm::vec3 color;       // 12
        float     intensity;   // 4
    };  // 32 bytes

    struct LightUniforms {
        DirectionalLightData dirLight;
        PointLightData       pointLights[64];
        int                  numPointLights;
        int                  _pad[3];
    };
}
