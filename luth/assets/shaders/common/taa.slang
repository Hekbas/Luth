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

// Blackman-Harris 3.3 weights (Karis reconstruction-filter slide). Sum ~= 1.0.
public static const float k_BlackmanHarris3x3[9] = {
    0.0094, 0.1145, 0.0094,
    0.1145, 0.5000, 0.1145,
    0.0094, 0.1145, 0.0094
};
