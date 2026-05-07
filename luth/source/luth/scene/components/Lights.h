#pragma once

#include "luth/core/types/LuthMath.h"

namespace Luth::Component
{
    struct DirectionalLight {
        Vec3 Color = Vec3(1.0f);
        float Intensity = 1.0f;
        bool CastShadows = true;
        float ShadowOrthoSize = 200.0f;
        float ShadowDistance = 200.0f;

        // Cascaded Shadow Map tunables.
        float SplitLambda = 0.5f;                                        // 0 = uniform, 1 = logarithmic (practical split)
        float ShadowBias[4]       = { 0.005f, 0.008f, 0.012f, 0.02f };   // per-cascade constant depth bias (NDC)
        float ShadowNormalBias[4] = { 1.0f, 1.0f, 1.0f, 1.0f };          // per-cascade normal offset (shader scales by texel size)
        bool  StabilizeCascades = true;                                  // texel-snap ortho origin to kill shimmer
        float CascadeBlendWidth = 0.2f;                                  // fraction of slice depth for cross-cascade blend (0–1)
        bool  DebugVisualizeCascades = false;                            // tint fragments by cascade index
    };

    struct PointLight {
        Vec3 Color = Vec3(1.0f);
        float Intensity = 1.0f;
        float Range = 350.0f;
    };
}
