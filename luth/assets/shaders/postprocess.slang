// Post-process composite: chromatic aberration + bloom + exposure + tonemap (ACES/Uncharted2/AgX) +
// contrast/saturation/vignette/grain + sRGB encode. Standalone Set 0 (own PostProcessUBO at b2).
[[vk::binding(0, 0)]] Sampler2D u_SceneColor;   // HDR
[[vk::binding(1, 0)]] Sampler2D u_BloomColor;

struct PostProcessUBO
{
    float bloomThreshold, bloomStrength, exposure, contrast;
    float saturation; int tonemapOp; float vignetteAmount, vignetteHardness;
    float grainAmount, sharpness, chromaticAberration, time;
    float3 shadowBalance;
    float3 midtoneBalance;
    float3 highlightBalance;
};
[[vk::binding(2, 0)]] ConstantBuffer<PostProcessUBO> pp;

struct FIn { [[vk::location(0)]] float2 uv; };

float3 ACESFilm(float3 x)
{
    float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}
float3 Reinhard(float3 x) { return x / (1.0 + x); }
float3 Uncharted2Helper(float3 x)
{
    float A = 0.15, B = 0.50, C = 0.10, D = 0.20, E = 0.02, F = 0.30;
    return ((x * (A * x + C * B) + D * E) / (x * (A * x + B) + D * F)) - E / F;
}
float3 Uncharted2(float3 x) { return Uncharted2Helper(x * 2.0) / Uncharted2Helper(float3(11.2)); }

// CONTRACT: AgX returns LINEAR sRGB; the pow(color, 1/2.2) tail does the display encode. The inner
// pow(color, 2.2) is a *decode* (sigmoid output sits in sRGB-OETF space). Wrensch fit / Blender 4.0+ AgX.
float3 agxSigmoid(float3 x)
{
    float3 x2 = x * x;
    float3 x4 = x2 * x2;
    return 15.5 * x4 * x2 - 40.14 * x4 * x + 31.96 * x4 - 6.868 * x2 * x + 0.4298 * x2 + 0.1191 * x - 0.00232;
}
float3 agxLookPunchy(float3 val)
{
    const float3 lw = float3(0.2126, 0.7152, 0.0722);
    float luma = dot(val, lw);
    val = pow(val, float3(1.35));   // slope 1.0, power 1.35
    return luma + 1.4 * (val - luma);
}
float3 AgXBase(float3 color, bool punchy)
{
    // GLSL mat3(c0,c1,c2) is column-major -> build float3x3 with the same vecs as rows + use mul(vec, M).
    const float3x3 AgXInset = float3x3(
        float3(0.856627153315983,  0.0951212405381588, 0.0482516061458583),
        float3(0.137318972929847,  0.761241990602591,  0.101439036467562),
        float3(0.11189821299995,   0.0767994186031903, 0.811302368396859));
    const float3x3 AgXOutset = float3x3(
        float3( 1.1271005818144368,   -0.1413297634984383,  -0.14132976349843826),
        float3(-0.11060664309660323,   1.157823702216272,   -0.11060664309660294),
        float3(-0.016493938717834573, -0.016493938717834257, 1.2519364065950405));
    const float AgxMinEv = -12.47393, AgxMaxEv = 4.026069;

    color = mul(color, AgXInset);
    color = max(color, float3(1e-10));
    color = log2(color);
    color = (color - AgxMinEv) / (AgxMaxEv - AgxMinEv);
    color = clamp(color, 0.0, 1.0);
    color = agxSigmoid(color);
    if (punchy) color = agxLookPunchy(color);
    color = mul(color, AgXOutset);
    color = pow(max(color, float3(0.0)), float3(2.2));   // sRGB EOTF decode; see CONTRACT
    return color;
}

[shader("fragment")]
float4 main(FIn i) : SV_Target
{
    float2 uv = i.uv;

    // Debug shade modes write display-ready DATA (encoded normals, IDs, [0,1] channels) into
    // SceneColor; a negative tonemapOp means "skip bloom + grade + tonemap, just sRGB-encode".
    if (pp.tonemapOp < 0)
        return float4(pow(max(u_SceneColor.Sample(uv).rgb, float3(0.0)), float3(1.0 / 2.2)), 1.0);

    float3 color;
    if (pp.chromaticAberration > 0.0)
    {
        float2 dir = uv - float2(0.5);
        float2 offset = dir * length(dir) * pp.chromaticAberration;
        color.r = u_SceneColor.Sample(uv + offset).r;
        color.g = u_SceneColor.Sample(uv).g;
        color.b = u_SceneColor.Sample(uv - offset).b;
    }
    else
    {
        color = u_SceneColor.Sample(uv).rgb;
    }

    if (pp.bloomStrength > 0.0)
        color += u_BloomColor.Sample(uv).rgb * pp.bloomStrength;

    color *= pp.exposure;

    if (pp.tonemapOp == 1)      color = Reinhard(color);
    else if (pp.tonemapOp == 2) color = ACESFilm(color);
    else if (pp.tonemapOp == 3) color = Uncharted2(color);
    else if (pp.tonemapOp == 4) color = AgXBase(color, false);
    else if (pp.tonemapOp == 5) color = AgXBase(color, true);

    color = lerp(float3(0.18), color, pp.contrast);

    float luma = dot(color, float3(0.2126, 0.7152, 0.0722));
    color = lerp(float3(luma), color, pp.saturation);

    if (pp.vignetteAmount > 0.0)
    {
        float dist = length(uv - float2(0.5)) * 1.414;
        float vig = smoothstep(1.0 - pp.vignetteHardness, 1.0, dist);
        color *= 1.0 - vig * pp.vignetteAmount;
    }

    if (pp.grainAmount > 0.0)
    {
        float noise = frac(sin(dot(uv * pp.time, float2(12.9898, 78.233))) * 43758.5453);
        color += (noise - 0.5) * pp.grainAmount;
    }

    color = pow(max(color, float3(0.0)), float3(1.0 / 2.2));
    return float4(color, 1.0);
}
