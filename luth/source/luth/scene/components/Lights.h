#pragma once

#include "luth/core/types/LuthMath.h"
#include "luth/renderer/lighting/LightTypes.h"  // ShadowingMode enum

namespace Luth::Component
{
    struct DirectionalLight {
        Vec3 Color = Vec3(1.0f);
        float Intensity = 1.0f;
        bool CastShadows = true;
        float ShadowOrthoSize = 200.0f;
        float ShadowDistance = 200.0f;

        // Sun-shadow path. Default RT; CSM kept as opt-in compare mode (see ShadowingMode).
        ShadowingMode Shadowing = ShadowingMode::RtShadows;

        // Cascaded Shadow Map tunables (consumed when Shadowing == RasterCSM).
        float SplitLambda = 0.5f;                                        // 0 = uniform, 1 = logarithmic (practical split)
        float ShadowBias[4]       = { 0.005f, 0.008f, 0.012f, 0.02f };   // per-cascade constant depth bias (NDC)
        float ShadowNormalBias[4] = { 1.0f, 1.0f, 1.0f, 1.0f };          // per-cascade normal offset (shader scales by texel size)
        bool  StabilizeCascades = true;                                  // texel-snap ortho origin to kill shimmer
        float CascadeBlendWidth = 0.2f;                                  // fraction of slice depth for cross-cascade blend (0–1)
        bool  DebugVisualizeCascades = false;                            // tint fragments by cascade index

        // RT-shadow tunables (consumed when Shadowing == RtShadows). World-space epsilons differ
        // semantically from the CSM NDC biases above — kept as separate fields to avoid overload.
        float RtOriginEpsilon  = 0.001f;
        float RtNormalEpsilon  = 0.05f;
    };

    struct PointLight {
        Vec3 Color = Vec3(1.0f);
        float Intensity = 1.0f;
        float Range = 350.0f;
    };

    // Beam direction is the entity's -Z axis (same convention as DirectionalLight). Cone angles are
    // half-angles from that axis; inner is the full-bright cap, outer the cutoff. Unshadowed MVP.
    struct SpotLight {
        Vec3 Color = Vec3(1.0f);
        float Intensity = 1.0f;
        float Range = 350.0f;
        float InnerConeAngleDeg = 25.0f;
        float OuterConeAngleDeg = 45.0f;
    };
}
