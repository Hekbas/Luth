#version 450
#extension GL_GOOGLE_include_directive : enable

// Karis14 YCoCg-clip TAA resolve. Recipe per:
//   - Karis, SIGGRAPH 2014 (rounded 3×3, clip>clamp, YCoCg, Blackman-Harris, luma feedback)
//   - Pedersen, GDC 2016 INSIDE (closest-depth velocity dilation, off-screen rejection)
//   - playdead/temporal (clip_aabb + RGB_YCoCg helpers, MIT-licensed)
// Pipeline: 9-tap 3×3 → YCoCg → rounded min/max (box + plus avg) → chroma narrow →
// closest-depth motion → off-screen reject → clip_aabb → luma feedback → blend → YCoCg→RGB.

#include "common/taa.glsl"

layout(location = 0) in vec2 v_TexCoord;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D currentColor;
layout(set = 0, binding = 1) uniform sampler2D motionVectors;
layout(set = 0, binding = 2) uniform sampler2D historyPrev;
layout(set = 0, binding = 3) uniform sampler2D sceneDepth;
// Binding 4 (UBO) is declared in the descriptor set layout but unused here — reserved for future
// TAA tunables (e.g. Salvi K4 variance gamma). Push constant carries the params we need today.

// invariant: jitterDeltaUv = (currentJitter − prevJitter) / viewportSize, populated by
// AddTaaResolvePass from ViewResources. Added to the resolve's per-pixel motion so static
// scenes resolve to motion = 0 instead of a sub-pixel jitter delta — without it bilinear
// history sampling at the Halton offset never converges. Production engines (UE/HDRP/INSIDE)
// de-jitter at the motion-vector producer; doing it here keeps slim_gbuffer untouched.
layout(push_constant) uniform TaaPC {
    float temporalAlpha;  // 0.05..0.2 — current-frame feedback weight in YCoCg blend
    float pad;
    vec2  jitterDeltaUv;
} taa;

void main()
{
    vec2 texSize  = vec2(textureSize(currentColor, 0));
    vec2 invSize  = 1.0 / texSize;

    // 1. Velocity dilation — closest-depth in 3×3. Makes silhouette edges follow the OCCLUDER's
    //    motion (front object), not the background. Fixes disocclusion ghosting that clamp alone
    //    can't address.
    vec2  closestUv = v_TexCoord;
    float minDepth  = 1e9;
    for (int y = -1; y <= 1; ++y)
    for (int x = -1; x <= 1; ++x)
    {
        vec2  sUv = v_TexCoord + vec2(x, y) * invSize;
        float d   = texture(sceneDepth, sUv).r;
        if (d < minDepth) { minDepth = d; closestUv = sUv; }
    }
    // Slim G-buffer motion = currNDC − prevNDC (NDC range ±2). ×0.5 → UV delta. The +jitterDeltaUv
    // adds back (currJitter − prevJitter)/viewport so static scenes net to zero motion; without it
    // prevUv lands sub-pixel-offset every frame and bilinear history sampling can't converge.
    vec2 motion = texture(motionVectors, closestUv).rg * 0.5 + taa.jitterDeltaUv;
    vec2 prevUv = v_TexCoord - motion;

    // 2. Sample 3×3 current neighborhood in YCoCg + accumulate Blackman-Harris reconstructed
    //    center. The BH-weighted sum IS the "current" pixel we blend with history — sharper than
    //    bilinear sampling of currentColor at the fragment center.
    vec3 ycc[9];
    vec3 currentBH = vec3(0.0);
    int  idx = 0;
    for (int y = -1; y <= 1; ++y)
    for (int x = -1; x <= 1; ++x)
    {
        vec3 rgb  = texture(currentColor, v_TexCoord + vec2(x, y) * invSize).rgb;
        ycc[idx]  = RGB_YCoCg(rgb);
        currentBH += ycc[idx] * k_BlackmanHarris3x3[idx];
        ++idx;
    }

    // 3. Rounded min/max — average of 3×3-box bounds and 5-tap-plus bounds (Karis). Reduces box
    //    artifacts plain 3×3 min/max produces around contrasty edges.
    vec3 minBox = ycc[0], maxBox = ycc[0];
    for (int i = 1; i < 9; ++i)
    {
        minBox = min(minBox, ycc[i]);
        maxBox = max(maxBox, ycc[i]);
    }
    // Plus = center + 4 cardinals (indices 4, 1, 3, 5, 7).
    vec3 minPlus = min(min(min(min(ycc[4], ycc[1]), ycc[3]), ycc[5]), ycc[7]);
    vec3 maxPlus = max(max(max(max(ycc[4], ycc[1]), ycc[3]), ycc[5]), ycc[7]);
    vec3 aabbMin = 0.5 * (minBox + minPlus);
    vec3 aabbMax = 0.5 * (maxBox + maxPlus);

    // 4. Chroma narrow — tighten chroma bounds independently to suppress the purple-fringe ghost
    //    that grayscale-tight luma + permissive chroma exhibits on saturated edges.
    float chromaExtent = 0.25 * 0.5 * (aabbMax.x - aabbMin.x);
    vec2  chromaCenter = ycc[4].yz;
    aabbMin.yz = chromaCenter - vec2(chromaExtent);
    aabbMax.yz = chromaCenter + vec2(chromaExtent);

    // 5. Off-screen UV rejection — frame 0 settles via this path (prev VP is identity → motion
    //    vectors are huge → reprojection lands outside [0,1]). Also handles disocclusion of
    //    newly revealed pixels.
    if (prevUv.x < 0.0 || prevUv.x > 1.0 || prevUv.y < 0.0 || prevUv.y > 1.0)
    {
        outColor = vec4(YCoCg_RGB(currentBH), 1.0);
        return;
    }

    // 6. Sample history + clip_aabb toward AABB center.
    vec4 historyRGBA = texture(historyPrev, prevUv);
    vec3 historyYCC  = RGB_YCoCg(historyRGBA.rgb);
    vec4 aabbCenter  = vec4(0.5 * (aabbMin + aabbMax), 1.0);
    vec4 clipped     = clip_aabb(aabbMin, aabbMax, vec4(historyYCC, 1.0), aabbCenter);

    // 7. Luma-distance feedback weight (Karis anti-flicker). Lerps between heavy history use when
    //    luma matches and lighter history use when it diverges. Squared curve gives a soft falloff
    //    so the clamp doesn't immediately flip to all-current.
    float lumCurr   = currentBH.x;
    float lumHist   = clipped.x;
    float lumDiff   = abs(lumCurr - lumHist);
    float lumScale  = max(max(lumCurr, lumHist), 0.2);
    float lumWeight = clamp(1.0 - lumDiff / lumScale, 0.0, 1.0);
    float feedback  = mix(0.2, taa.temporalAlpha, lumWeight * lumWeight);

    // 8. Blend in YCoCg then convert back to RGB.
    vec3 result = mix(clipped.xyz, currentBH, feedback);
    outColor    = vec4(YCoCg_RGB(result), 1.0);
}
