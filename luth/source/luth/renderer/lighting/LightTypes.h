#pragma once

#include "luth/core/types/LuthMath.h"

#include <vector>


namespace Luth
{
    // Cascaded shadow maps for directional lights. Cascade count and per-cascade resolution
    // must stay in sync with the GLSL counterparts in pbr.frag and shadowDepth.* shaders.
    inline constexpr u32 k_ShadowCascadeCount = 4;
    inline constexpr u32 k_ShadowResolution   = 2048;

    // Sun shadowing path selector. RT is the default once the rt-shadows effort lands; CSM stays
    // as opt-in compare mode. Both paths consume identical inputs (sun direction, world-space
    // position, surface normal) and produce a per-fragment shadow factor in [0,1]; the dispatch
    // happens at the pbr.frag::ComputeShadow wrapper. Mirrors the TonemapOperator compare-mode
    // precedent in PostProcessSettings.
    enum class ShadowingMode : i32
    {
        RasterCSM = 0,
        RtShadows = 1,
    };

    // ── Light data structs (mirrored in pbr.frag Set 3 LightSSBO) ──

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

    // Three vec3 force three 16-B slots under std430; the 4th scalar (cosOuter) needs a fourth → 64 B.
    // First 48 B mirror PointLightData's vec3+float pairing; cosOuter + pad ride the last slot.
    // Cone cosines (not angles) so the shader's smoothstep edge test is a bare dot-product compare.
    struct SpotLightData {
        Vec3 position;    // 0
        float     range;       // 12
        Vec3 direction;   // 16  (beam axis, normalized)
        float     cosInner;    // 28
        Vec3 color;       // 32
        float     intensity;   // 44
        float     cosOuter;    // 48
        float     _pad[3];     // 52
    };  // 64 bytes

    // CPU-side aggregate produced by LightGatherer each game stage. Held on LightingSystem;
    // the light vectors reuse capacity across frames so steady-state allocations stay flat.
    struct GatheredLights {
        DirectionalLightData        dirLight{};
        std::vector<PointLightData> points;
        std::vector<SpotLightData>  spots;
    };

    // ── Forward+ clustered lighting ──
    // invariant: cluster tile + slice counts must match the GLSL constants in cluster_build.comp,
    // light_assign.comp, and pbr.frag's ComputeClusterID. SSBO bindings on Set 3 b0-b2 (see arch/rendering-pipeline.md).

    inline constexpr u32 k_ClusterTilesX        = 16;
    inline constexpr u32 k_ClusterTilesY        =  9;
    // 48 slices sized so each cluster spans ~2.7 atlas slices (128 atlas slices total) — keeps
    // fog Z-banding below the visible threshold.
    inline constexpr u32 k_ClusterSlicesZ       = 48;
    inline constexpr u32 k_ClusterCount         = k_ClusterTilesX * k_ClusterTilesY * k_ClusterSlicesZ;  // 6912
    inline constexpr u32 k_MaxLightsPerCluster  = 128;

    // Set 3 binding 0 layout: { LightSSBOHeader; PointLightData points[pointLightCount]; SpotLightData spots[spotLightCount]; }
    // One contiguous tagged-heap region per frame — header at 0, points at 48, spots at 48 + pointCount*32.
    // PointLightData / DirectionalLightData are already std430-compatible (vec3 + float pairs in 16B slots).
    struct LightSSBOHeader {
        DirectionalLightData dirLight;          // 32 B
        u32                  pointLightCount;   //  4 (offset 32)
        u32                  spotLightCount;    //  4 (offset 36)
        u32                  _pad[2];           //  8 (std430 array boundary — points[] starts at offset 48)
    };
    static_assert(sizeof(LightSSBOHeader) == 48, "LightSSBOHeader std430 layout");
    static_assert(sizeof(DirectionalLightData) == 32, "DirectionalLightData std430 layout");
    static_assert(sizeof(PointLightData)       == 32, "PointLightData std430 layout");
    static_assert(sizeof(SpotLightData)        == 64, "SpotLightData std430 layout");

    // Set 3 binding 1 element. uvec2 (offset, count) into the LightIndexSSBO range for one cluster.
    struct GPUCluster {
        u32 offset;
        u32 count;
    };
    static_assert(sizeof(GPUCluster) == 8, "GPUCluster std430 layout");

    // Per-frame directional-light shadow config, snapshot from the first
    // Component::DirectionalLight each frame. Sticky — last-known values remain
    // when no directional light is present.
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

        // RT-shadows path (consumed when mode == RtShadows). RT epsilons are world-space, semantically
        // distinct from CSM's shadowBias/shadowNormalBias (which are NDC depth biases). See Wächter &
        // Binder 2019 "A Fast and Robust Method for Avoiding Self-Intersection" (RT Gems Ch. 6).
        ShadowingMode mode             = ShadowingMode::RtShadows;
        float         rtOriginEpsilon  = 0.001f;   // World-space offset along sun direction (avoids self-int at origin)
        float         rtNormalEpsilon  = 0.05f;    // Normal-offset multiplier scaled by (1-NdotL) for grazing-angle correctness
    };

    // CSM output produced each frame. PBR shader needs view-Z splits + world-space
    // texel size (for normal-bias scaling) alongside the light-space matrices.
    struct CascadeData
    {
        Mat4 lightSpaceMatrix[k_ShadowCascadeCount] = {
            Mat4(1.0f), Mat4(1.0f), Mat4(1.0f), Mat4(1.0f)
        };
        Vec4 splitsViewZ = Vec4(0.0f);  // Per-cascade far view-Z (absolute)
        Vec4 texelSize   = Vec4(1.0f);  // World-space size of one shadow texel per cascade
    };
}
