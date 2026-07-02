// PPLL node layout + packing; twin of oit_common.glsl. Single source for the 16 B node (C++ sizes the
// pool as 16 + W*H*budget*16). see arch/rendering-pipeline.md

public static const uint OIT_EMPTY = 0xFFFFFFFFu;

// depthAlpha = (depth24 << 8) | alpha8: one u32 sort key (ascending = nearest-first; alpha bits break
// exact-depth ties). entityID keeps its full 32 bits (entt version bits; never truncate).
public struct OITNode
{
    public uint colorRGB9E5;   // shaded + fogged radiance (RGB9E5 shared-exponent HDR)
    public uint depthAlpha;    // (fixed24(fragCoord.z) << 8) | alpha8
    public uint entityID;
    public uint next;          // previous head index; OIT_EMPTY terminates
}

public uint OitPackDepthAlpha(float depth01, float alpha)
{
    uint d24 = uint(clamp(depth01, 0.0, 1.0) * 16777215.0);
    uint a8  = uint(clamp(alpha, 0.0, 1.0) * 255.0 + 0.5);
    return (d24 << 8) | a8;
}
public float OitUnpackAlpha(uint depthAlpha) { return float(depthAlpha & 0xFFu) * (1.0 / 255.0); }

// RGB9E5 shared-exponent pack (Khronos layout: 9-bit mantissas, 5-bit exponent, bias 15).
public uint OitPackRGB9E5(float3 c)
{
    const float kMax = 65408.0;   // (511/512) * 2^16, format ceiling
    float3 v    = clamp(c, float3(0.0), float3(kMax));
    float  maxC = max(v.r, max(v.g, v.b));
    int    e    = (maxC < 1e-7) ? 0 : clamp(int(floor(log2(maxC))) + 16, 0, 31);
    float  invScale = exp2(float(24 - e));
    uint3  m    = min(uint3(v * invScale + 0.5), uint3(511u));
    return (uint(e) << 27) | (m.b << 18) | (m.g << 9) | m.r;
}
public float3 OitUnpackRGB9E5(uint p)
{
    float scale = exp2(float(int(p >> 27) - 24));
    return float3(float(p & 0x1FFu), float((p >> 9) & 0x1FFu), float((p >> 18) & 0x1FFu)) * scale;
}
