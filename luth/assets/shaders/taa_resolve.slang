// Karis14 YCoCg-clip TAA resolve. 3x3 -> YCoCg -> rounded min/max -> chroma narrow -> closest-depth
// motion (matrix-reprojected sky fallback) -> off-screen reject -> Catmull-Rom history + NaN sanitize ->
// clip_aabb -> luma feedback -> Karis 1/(1+luma)-weighted HDR blend. see arch/rendering-pipeline.md
import taa;

[[vk::binding(0, 0)]] Sampler2D currentColor;
[[vk::binding(1, 0)]] Sampler2D motionVectors;
[[vk::binding(2, 0)]] Sampler2D historyPrev;
[[vk::binding(3, 0)]] Sampler2D sceneDepth;

struct PC
{
    float4x4 skyReproj;      // un-jittered prevVP * inv(currVP); reprojects depth == 1 (no raster motion)
    float    temporalAlpha;
};
[[vk::push_constant]] PC taa;

struct FIn { [[vk::location(0)]] float2 uv; };

[shader("fragment")]
float4 main(FIn i) : SV_Target
{
    uint w, h; currentColor.GetDimensions(w, h);
    float2 invSize = 1.0 / float2(w, h);

    // 1. Closest-depth velocity dilation in 3x3.
    float2 closestUv = i.uv;
    float  minDepth  = 1e9;
    for (int y = -1; y <= 1; ++y)
    for (int x = -1; x <= 1; ++x)
    {
        float2 sUv = i.uv + float2(x, y) * invSize;
        float  d   = sceneDepth.Sample(sUv).r;
        if (d < minDepth) { minDepth = d; closestUv = sUv; }
    }
    float2 motion = motionVectors.Sample(closestUv).rg * 0.5;
    float2 prevUv = i.uv - motion;

    // Sky fallback: depth == 1 rasterizes no motion vector, so a rotating camera smears the skybox.
    // Reproject through the un-jittered VP pair instead; w <= 0 (behind camera) keeps the raster path.
    if (minDepth >= 1.0)
    {
        float4 pC = mul(taa.skyReproj, float4(i.uv * 2.0 - 1.0, 1.0, 1.0));
        if (pC.w > 0.0) prevUv = (pC.xy / pC.w) * 0.5 + 0.5;
    }

    // 2. 3x3 current neighbourhood in YCoCg + Blackman-Harris reconstructed center.
    float3 ycc[9];
    float3 currentBH = float3(0.0);
    int idx = 0;
    for (int y = -1; y <= 1; ++y)
    for (int x = -1; x <= 1; ++x)
    {
        float3 rgb = currentColor.Sample(i.uv + float2(x, y) * invSize).rgb;
        ycc[idx]   = RGB_YCoCg(rgb);
        currentBH += ycc[idx] * k_BlackmanHarris3x3[idx];
        ++idx;
    }

    // 3. Rounded min/max (box + plus average).
    float3 minBox = ycc[0], maxBox = ycc[0];
    for (int j = 1; j < 9; ++j) { minBox = min(minBox, ycc[j]); maxBox = max(maxBox, ycc[j]); }
    float3 minPlus = min(min(min(min(ycc[4], ycc[1]), ycc[3]), ycc[5]), ycc[7]);
    float3 maxPlus = max(max(max(max(ycc[4], ycc[1]), ycc[3]), ycc[5]), ycc[7]);
    float3 aabbMin = 0.5 * (minBox + minPlus);
    float3 aabbMax = 0.5 * (maxBox + maxPlus);

    // 4. Chroma narrow.
    float chromaExtent = 0.25 * 0.5 * (aabbMax.x - aabbMin.x);
    float2 chromaCenter = ycc[4].yz;
    aabbMin.yz = chromaCenter - float2(chromaExtent);
    aabbMax.yz = chromaCenter + float2(chromaExtent);

    // 5. Off-screen UV rejection (settles frame 0 + disocclusion).
    if (prevUv.x < 0.0 || prevUv.x > 1.0 || prevUv.y < 0.0 || prevUv.y > 1.0)
        return float4(max(YCoCg_RGB(currentBH), float3(0.0)), 1.0);

    // 6. History: Catmull-Rom resample (bilinear-only resampling low-passes the EMA every frame), NaN/inf
    //    sanitize (a poisoned texel otherwise sticks in the feedback loop and spreads via the taps), clip.
    float3 historyYCC = RGB_YCoCg(SampleHistoryCatmullRom(historyPrev, prevUv, invSize));
    if (any(isnan(historyYCC)) || any(isinf(historyYCC))) historyYCC = currentBH;
    float4 aabbCenter = float4(0.5 * (aabbMin + aabbMax), 1.0);
    float4 clipped    = clip_aabb(aabbMin, aabbMax, float4(historyYCC, 1.0), aabbCenter);

    // 7. Luma-distance feedback weight (Karis anti-flicker).
    float lumCurr   = currentBH.x;
    float lumHist   = clipped.x;
    float lumDiff   = abs(lumCurr - lumHist);
    float lumScale  = max(max(lumCurr, lumHist), 0.2);
    float lumWeight = clamp(1.0 - lumDiff / lumScale, 0.0, 1.0);
    float feedback  = lerp(0.2, taa.temporalAlpha, lumWeight * lumWeight);

    // 8. Karis HDR anti-flicker blend: 1/(1+luma) tonemap weights keep a single bright HDR spike from
    //    dominating the linear EMA (unweighted, highlights pulse and sit above the stable mean).
    float wC = feedback         / (1.0 + max(currentBH.x, 0.0));
    float wH = (1.0 - feedback) / (1.0 + max(clipped.x, 0.0));
    float3 result = (currentBH * wC + clipped.xyz * wH) / max(wC + wH, 1e-6);
    // Clamp: chroma-narrowed clip + Catmull-Rom undershoot can go slightly negative, and this image
    // feeds bloom's Karis average + the next frame's history.
    return float4(max(YCoCg_RGB(result), float3(0.0)), 1.0);
}
