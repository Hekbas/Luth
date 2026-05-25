#pragma once

#include "luth/core/types/LuthMath.h"

#include <array>

namespace Luth::TAA
{
    // Halton(2,3) prefix of 8 sub-pixel offsets in [-0.5, +0.5]². Matches Karis 2014 + UE4/UE5 base
    // TAA default. Pairs with tight YCoCg variance clip — longer sequences hurt convergence under
    // aggressive clipping. INSIDE's N=16 was paired with looser RGB min/max; opposite pairing.
    //
    // Halton(b, i) for i=1..8:
    //   base 2: 1/2, 1/4, 3/4, 1/8, 5/8, 3/8, 7/8, 1/16
    //   base 3: 1/3, 2/3, 1/9, 4/9, 7/9, 2/9, 5/9, 8/9
    // Offsets are (haltonX - 0.5, haltonY - 0.5).
    inline constexpr std::array<Vec2, 8> k_HaltonJitter = {
        Vec2( 0.5f    - 0.5f,  1.0f/3.0f - 0.5f),
        Vec2( 0.25f   - 0.5f,  2.0f/3.0f - 0.5f),
        Vec2( 0.75f   - 0.5f,  1.0f/9.0f - 0.5f),
        Vec2( 0.125f  - 0.5f,  4.0f/9.0f - 0.5f),
        Vec2( 0.625f  - 0.5f,  7.0f/9.0f - 0.5f),
        Vec2( 0.375f  - 0.5f,  2.0f/9.0f - 0.5f),
        Vec2( 0.875f  - 0.5f,  5.0f/9.0f - 0.5f),
        Vec2( 0.0625f - 0.5f,  8.0f/9.0f - 0.5f),
    };

    inline Vec2 SampleHalton(u64 frameAbs)
    {
        return k_HaltonJitter[frameAbs % k_HaltonJitter.size()];
    }

    // Sub-pixel jitter on the projection matrix: shifts NDC by ±jitter/halfViewport. Karis recipe —
    // rendered pixels land at non-grid positions; motion vectors naturally absorb the jitter delta;
    // the resolve pass reads jittered current + reprojected history and integrates.
    inline Mat4 ApplyJitter(const Mat4& proj, Vec2 jitter, u32 w, u32 h)
    {
        Mat4 j = proj;
        j[2][0] += jitter.x * 2.0f / static_cast<f32>(w);
        j[2][1] += jitter.y * 2.0f / static_cast<f32>(h);
        return j;
    }
}
