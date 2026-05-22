#pragma once

#include "luth/core/types/LuthMath.h"


namespace Luth
{
    // Cascaded shadow maps for directional lights. Cascade count and per-cascade resolution
    // must stay in sync with the GLSL counterparts in pbr.frag and shadowDepth.* shaders.
    inline constexpr u32 k_ShadowCascadeCount = 4;
    inline constexpr u32 k_ShadowResolution   = 2048;

    // ── Light data structs (mirrored in pbr.frag Set 3) ──

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

    // ── Forward+ clustered lighting ──
    // invariant: cluster tile + slice counts must match the GLSL constants in cluster_build.comp,
    // light_assign.comp, and pbr.frag's ComputeClusterID. SSBO bindings on Set 3 b0-b2 (see arch/rendering-pipeline.md).

    inline constexpr u32 k_ClusterTilesX        = 16;
    inline constexpr u32 k_ClusterTilesY        =  9;
    inline constexpr u32 k_ClusterSlicesZ       = 24;
    inline constexpr u32 k_ClusterCount         = k_ClusterTilesX * k_ClusterTilesY * k_ClusterSlicesZ;  // 3456
    inline constexpr u32 k_MaxLightsPerCluster  = 128;

    // Set 3 binding 0 layout: { LightSSBOHeader header; PointLightData points[header.pointLightCount]; }
    // Allocated as one contiguous tagged-heap region per frame — header at offset 0, points immediately after.
    // PointLightData / DirectionalLightData are already std430-compatible (vec3 + float pairs in 16B slots).
    struct LightSSBOHeader {
        DirectionalLightData dirLight;          // 32 B
        u32                  pointLightCount;   //  4
        u32                  _pad[3];           // 12 (std430 array boundary — points[] starts at offset 48)
    };
    static_assert(sizeof(LightSSBOHeader) == 48, "LightSSBOHeader std430 layout");
    static_assert(sizeof(DirectionalLightData) == 32, "DirectionalLightData std430 layout");
    static_assert(sizeof(PointLightData)       == 32, "PointLightData std430 layout");

    // Set 3 binding 1 element. uvec2 (offset, count) into the LightIndexSSBO range for one cluster.
    struct GPUCluster {
        u32 offset;
        u32 count;
    };
    static_assert(sizeof(GPUCluster) == 8, "GPUCluster std430 layout");

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
