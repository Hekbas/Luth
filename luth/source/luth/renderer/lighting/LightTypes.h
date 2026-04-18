#pragma once

#include "luth/core/types/LuthMath.h"


namespace Luth
{
    // Number of shadow cascades for directional-light CSM (Phase 13)
    inline constexpr u32 k_ShadowCascadeCount = 4;
    inline constexpr u32 k_ShadowResolution   = 2048;

    // ---- Light data structs (mirrored in pbr.frag Set 3) ----

    struct DirectionalLightData {
        Vec3 direction;   // 12
        float     intensity;   // 4
        Vec3 color;       // 12
        float     _pad;        // 4
    };  // 32 bytes

    struct PointLightData {
        Vec3 position;    // 12
        float     range;       // 4
        Vec3 color;       // 12
        float     intensity;   // 4
    };  // 32 bytes

    struct LightUniforms {
        DirectionalLightData dirLight;
        PointLightData       pointLights[64];
        int                  numPointLights;
        int                  _pad[3];
    };

    // Per-frame directional-light shadow config, snapshot from the first
    // Component::DirectionalLight each frame. Sticky — if no directional light
    // is present, last-known values remain. Feeds both CascadeBuilder (split
    // lambda / shadow distance / stabilization) and GlobalUniforms (biases +
    // blend width + debug visualize).
    struct DirectionalLightShadowParams
    {
        Vec4 shadowBias            = Vec4(0.005f, 0.008f, 0.012f, 0.02f);
        Vec4 shadowNormalBias      = Vec4(1.0f);
        float     splitLambda           = 0.5f;
        float     shadowDistance        = 200.0f;
        float     cascadeBlendWidth     = 0.2f;
        bool      castShadows           = true;
        bool      stabilizeCascades     = true;
        bool      debugVisualizeCascades = false;
    };

    // Per-frame CSM output — one light-space matrix per cascade plus derived
    // per-cascade data the PBR shader needs (far view-Z split + world-space
    // texel size for normal-bias scaling).
    struct CascadeData
    {
        Mat4 lightSpaceMatrix[k_ShadowCascadeCount] = {
            Mat4(1.0f), Mat4(1.0f), Mat4(1.0f), Mat4(1.0f)
        };
        Vec4 splitsViewZ = Vec4(0.0f);  // Per-cascade far view-Z (absolute)
        Vec4 texelSize   = Vec4(1.0f);  // World-space size of one shadow texel per cascade
    };
}
