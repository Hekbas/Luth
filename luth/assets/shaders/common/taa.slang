// Karis14 / Pedersen16 TAA helpers; twin of taa.glsl. YCoCg conversion + clip_aabb + Blackman-Harris
// reconstruction weights. see arch/rendering-pipeline.md

// YCoCg <-> RGB (BT.601). Chroma decorrelates from luma so the neighborhood clamp narrows chroma separately.
public float3 RGB_YCoCg(float3 c)
{
    return float3( 0.25 * c.r + 0.5 * c.g + 0.25 * c.b,
                   0.5  * c.r              - 0.5  * c.b,
                  -0.25 * c.r + 0.5 * c.g - 0.25 * c.b);
}
public float3 YCoCg_RGB(float3 c)
{
    float y = c.x, co = c.y, cg = c.z;
    return float3(y + co - cg, y + cg, y - co - cg);
}

// playdead clip_aabb: clip history toward the AABB center along the (history - center) ray.
public float4 clip_aabb(float3 aabb_min, float3 aabb_max, float4 p, float4 q)
{
    float3 p_clip = 0.5 * (aabb_max + aabb_min);
    float3 e_clip = 0.5 * (aabb_max - aabb_min) + float3(1e-7);
    float4 v_clip = p - float4(p_clip, q.w);
    float3 v_unit = v_clip.xyz / e_clip;
    float3 a_unit = abs(v_unit);
    float  ma_unit = max(max(a_unit.x, a_unit.y), a_unit.z);
    if (ma_unit > 1.0) return float4(p_clip, q.w) + v_clip / ma_unit;
    return p;
}

// Blackman-Harris 3.3 (Karis slide) renormalized to sum 1: raw values sum 0.9956 = 0.4% steady-state darkening.
public static const float k_BlackmanHarris3x3[9] = {
    0.009442, 0.115006, 0.009442,
    0.115006, 0.502210, 0.115006,
    0.009442, 0.115006, 0.009442
};

// Optimized 5-tap Catmull-Rom history resample (Jimenez 2016): drops the 4 corner taps, renormalized by the
// kept weight sum so DC is preserved exactly. Bilinear-only resampling low-passes the EMA every frame; this
// keeps accumulated detail sharp. Negative-lobe undershoot is left to the caller's output clamp.
public float3 SampleHistoryCatmullRom(Sampler2D tex, float2 uv, float2 texelSize)
{
    float2 samplePos = uv / texelSize;
    float2 tc1 = floor(samplePos - 0.5) + 0.5;
    float2 f   = samplePos - tc1;
    float2 w0  = f * (-0.5 + f * (1.0 - 0.5 * f));
    float2 w1  = 1.0 + f * f * (-2.5 + 1.5 * f);
    float2 w2  = f * (0.5 + f * (2.0 - 1.5 * f));
    float2 w3  = f * f * (-0.5 + 0.5 * f);
    float2 w12 = w1 + w2;
    float2 tc0  = (tc1 - 1.0) * texelSize;
    float2 tc3  = (tc1 + 2.0) * texelSize;
    float2 tc12 = (tc1 + w2 / w12) * texelSize;
    float3 c = tex.SampleLevel(float2(tc12.x, tc0.y),  0.0).rgb * (w12.x * w0.y)
             + tex.SampleLevel(float2(tc0.x,  tc12.y), 0.0).rgb * (w0.x  * w12.y)
             + tex.SampleLevel(float2(tc12.x, tc12.y), 0.0).rgb * (w12.x * w12.y)
             + tex.SampleLevel(float2(tc3.x,  tc12.y), 0.0).rgb * (w3.x  * w12.y)
             + tex.SampleLevel(float2(tc12.x, tc3.y),  0.0).rgb * (w12.x * w3.y);
    float wSum = w12.x * w0.y + w0.x * w12.y + w12.x * w12.y + w3.x * w12.y + w12.x * w3.y;
    return c / wSum;
}
