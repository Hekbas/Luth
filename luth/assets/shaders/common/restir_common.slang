// ReSTIR DI shared types + helpers (Bitterli 2020). Reservoir layout mirrors the C++ GPUReservoir
// (RtRestirSubsystem). see arch/rendering-pipeline.md

public struct DirectionalLightData
{
    public float3 direction;
    public float  intensity;
    public float3 color;
    public float  _pad;
}

public struct PointLightData
{
    public float3 position;
    public float  range;
    public float3 color;
    public float  intensity;
}

// 64 B, std430 (mirrors C++ SpotLightData). Cone stored as cosines of the half-angles.
public struct SpotLightData
{
    public float3 position;
    public float  range;
    public float3 direction;
    public float  cosInner;
    public float3 color;
    public float  intensity;
    public float  cosOuter;
    public float  _pad0, _pad1, _pad2;
}

// 32 B, std430. lightIndex bit 31 selects the light TYPE (0 = points[] index, 1 = triangle index in the
// low 31 bits); RESTIR_NO_LIGHT (all-ones) stays reserved. histDepth/histOctN self-carry the origin
// pixel's geometry (written by the spatial pass, read by temporal validation). uvPacked = the sampled
// triangle's barycentric uv (unorm16x2), re-instantiated at every reuse/shade site; 0 for point lights.
public struct Reservoir
{
    public uint  lightIndex;   // bit31 = type; low 31 = points[]/triangle index; RESTIR_NO_LIGHT = empty
    public float W;            // unbiased contribution weight (Bitterli 2020)
    public float wSum;         // running sum of resampling weights
    public uint  M;            // sample count / confidence
    public float targetPdf;    // p-hat of the selected sample
    public float histDepth;    // origin-pixel raw depth (temporal-validation self-carry)
    public uint  histOctN;     // origin-pixel oct normal, f16x2 (packed; float round-trip loses denormals)
    public uint  uvPacked;     // sampled triangle barycentric uv, unorm16x2; 0 for point lights
}

public static const uint RESTIR_NO_LIGHT = 0xFFFFFFFFu;

public void ReservoirReset(out Reservoir r)
{
    r.lightIndex = RESTIR_NO_LIGHT;
    r.W = 0.0; r.wSum = 0.0; r.M = 0u; r.targetPdf = 0.0;
    r.histDepth = 0.0; r.histOctN = 0u; r.uvPacked = 0u;
}

// Stream one candidate into the reservoir (weighted reservoir sampling, Chao 1982). uvPacked travels with
// the winning candidate (the triangle barycentric; 0 for points).
public void ReservoirUpdate(inout Reservoir r, uint lightIndex, uint uvPacked, float targetPdf, float w, float rnd)
{
    r.wSum += w;
    r.M    += 1u;
    if (r.wSum > 0.0 && rnd * r.wSum <= w) { r.lightIndex = lightIndex; r.uvPacked = uvPacked; r.targetPdf = targetPdf; }
}

public void ReservoirFinalize(inout Reservoir r)
{
    r.W = (r.targetPdf > 0.0 && r.M > 0u) ? (r.wSum / (float(r.M) * r.targetPdf)) : 0.0;
}

// Confidence-weighted spatiotemporal combine (Bitterli 2020); targetPdfAtCurr re-evaluated per pixel.
public void ReservoirMerge(inout Reservoir c, uint lightIndex, uint uvPacked, float targetPdfAtCurr, float Wsrc, uint Msrc, float rnd)
{
    float w = targetPdfAtCurr * Wsrc * float(Msrc);
    c.wSum += w;
    c.M    += Msrc;
    if (c.wSum > 0.0 && rnd * c.wSum <= w) { c.lightIndex = lightIndex; c.uvPacked = uvPacked; c.targetPdf = targetPdfAtCurr; }
}

// Selection-reporting merge for BASIC bias correction: returns true when this candidate is chosen, so the
// caller can record which domain won (needed for the pi / piSum generalized-balance normalization).
public bool ReservoirMergeSel(inout Reservoir c, uint lightIndex, uint uvPacked, float targetPdfAtCurr, float Wsrc, uint Msrc, float rnd)
{
    float w = targetPdfAtCurr * Wsrc * float(Msrc);
    c.wSum += w;
    c.M    += Msrc;
    if (c.wSum > 0.0 && rnd * c.wSum <= w) { c.lightIndex = lightIndex; c.uvPacked = uvPacked; c.targetPdf = targetPdfAtCurr; return true; }
    return false;
}

public float Luminance(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }

// PCG hash -> uint; RandFromSeed advances the stream and returns [0,1).
public uint PcgHash(uint v)
{
    uint state = v * 747796405u + 2891336453u;
    uint word  = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}
public float RandFromSeed(inout uint seed) { seed = PcgHash(seed); return float(seed) * (1.0 / 4294967296.0); }

// Octahedral decode: inverse of slim_gbuffer.frag OctEncode.
public float3 OctDecode(float2 enc)
{
    enc = enc * 2.0 - 1.0;
    float3 n = float3(enc.xy, 1.0 - abs(enc.x) - abs(enc.y));
    float t = max(-n.z, 0.0);
    n.x += (n.x >= 0.0) ? -t : t;
    n.y += (n.y >= 0.0) ? -t : t;
    return normalize(n);
}

// Pack/unpack an octahedral normal (RG16F-sourced, so f16 is lossless) into one uint for the reservoir
// self-carry. Mirrors the GI visNormalPacked convention; the packed field MUST be a uint (a float
// round-trip canonicalizes denormals). see arch/rendering-pipeline.md
public uint   PackOctN(float2 o)  { return f32tof16(o.x) | (f32tof16(o.y) << 16u); }
public float2 UnpackOctN(uint p)  { return float2(f16tof32(p), f16tof32(p >> 16u)); }

// Incoming radiance from a point light at worldPos; matches the raster attenuation + rolloff.
public float3 PointLightRadiance(PointLightData pl, float3 worldPos, out float3 L, out float dist)
{
    float3 toLight = pl.position - worldPos;
    dist = length(toLight);
    L    = (dist > 0.0) ? toLight / dist : float3(0.0, 1.0, 0.0);
    float atten   = 1.0 / max(dist * dist, 1e-4);
    float rolloff = pow(clamp(1.0 - dist / max(pl.range, 1e-4), 0.0, 1.0), 2.0);
    return pl.color * pl.intensity * atten * rolloff;
}

// ---- Unified local-light sampling (point lights + emissive triangles) ----
// lightIndex bit 31 = type (0 point / 1 triangle); low 31 bits = index into points[] / tris[].
public static const uint LIGHT_TYPE_TRI   = 0x80000000u;
public static const uint LIGHT_INDEX_MASK = 0x7FFFFFFFu;

// Emissive triangle light (world-space); mirrors C++ TriangleLightData (LightTypes.h). area drives the
// area-measure source pdf at draw time; the emitter normal is derived from cross(e1, e2).
public struct TriLight { public float3 v0; public float area; public float3 e1; public float3 e2; public float3 avgLe; };

// Re-instantiated incident sample toward a light: radiance, unit direction, distance to the sample point.
public struct LightSample { public float3 Li; public float3 L; public float dist; };

// Barycentric uv (coeffs of e1/e2, each in [0,1], sum <= 1) as 2x unorm16. Quantized ONCE at draw and
// re-instantiated identically at every reuse/shade site (the no-Jacobian contract).
public uint   PackUV(float2 uv) { return uint(saturate(uv.x) * 65535.0 + 0.5) | (uint(saturate(uv.y) * 65535.0 + 0.5) << 16u); }
public float2 UnpackUV(uint p)  { return float2(float(p & 0xFFFFu), float(p >> 16u)) * (1.0 / 65535.0); }

// LightSSBO layout (header: dir 32 + pointCount@32 + spotCount@36 + triCount@40; points@48 32B; spots@ 64B;
// tris@ 64B; alias@ 16B). Accessors take the per-shader g_Lights binding as an argument.
public uint LightPointCount(ByteAddressBuffer lb) { return lb.Load(32); }
public uint LightTriCount(ByteAddressBuffer lb)   { return lb.Load(40); }
uint TrisBaseBytes(ByteAddressBuffer lb)  { return 48u + lb.Load(32) * 32u + lb.Load(36) * 64u; }
uint AliasBaseBytes(ByteAddressBuffer lb) { return TrisBaseBytes(lb) + lb.Load(40) * 64u; }

public PointLightData LoadPointL(ByteAddressBuffer lb, uint j)
{
    uint b = 48u + j * 32u;
    PointLightData p;
    p.position  = asfloat(lb.Load3(b + 0u));  p.range     = asfloat(lb.Load(b + 12u));
    p.color     = asfloat(lb.Load3(b + 16u)); p.intensity = asfloat(lb.Load(b + 28u));
    return p;
}
public TriLight LoadTriL(ByteAddressBuffer lb, uint j)
{
    uint b = TrisBaseBytes(lb) + j * 64u;
    TriLight t;
    t.v0    = asfloat(lb.Load3(b + 0u));  t.area = asfloat(lb.Load(b + 12u));
    t.e1    = asfloat(lb.Load3(b + 16u)); t.e2   = asfloat(lb.Load3(b + 32u));
    t.avgLe = asfloat(lb.Load3(b + 48u));
    return t;
}
// Power-weighted alias draw over the unified [points | tris] list: bucket i + uniform u -> unified index;
// returns the selected light's pmf (power_i / total) via out param for the RIS source-pdf weight.
public uint AliasSelect(ByteAddressBuffer lb, uint i, float u, out float pmf)
{
    uint  ab   = AliasBaseBytes(lb);
    float prob = asfloat(lb.Load(ab + i * 16u + 0u));
    uint  alt  = lb.Load(ab + i * 16u + 4u);
    uint  sel  = (u < prob) ? i : alt;
    pmf = asfloat(lb.Load(ab + sel * 16u + 8u));
    return sel;
}

// Re-instantiate a light sample (point or triangle) at a receiver. Triangle: barycentric uv -> world point
// x_L, two-sided radiance Li = avgLe*|cosThetaL|/d^2 (area-measure incident radiance, structurally the same
// as a point light's intensity/d^2). see arch/rendering-pipeline.md
public LightSample ReSampleLight(ByteAddressBuffer lb, uint lightIndex, uint uvPacked, float3 receiverPos)
{
    LightSample s;
    if ((lightIndex & LIGHT_TYPE_TRI) != 0u)
    {
        TriLight t = LoadTriL(lb, lightIndex & LIGHT_INDEX_MASK);
        float2 uv  = UnpackUV(uvPacked);
        float3 xL  = t.v0 + uv.x * t.e1 + uv.y * t.e2;
        float3 d   = xL - receiverPos;
        float  d2  = max(dot(d, d), 1.0e-4);
        s.dist     = sqrt(d2);
        s.L        = d / s.dist;
        float3 nL  = normalize(cross(t.e1, t.e2));
        s.Li       = t.avgLe * (abs(dot(nL, s.L)) / d2);   // two-sided emitter
    }
    else
    {
        PointLightData p = LoadPointL(lb, lightIndex & LIGHT_INDEX_MASK);
        s.Li = PointLightRadiance(p, receiverPos, s.L, s.dist);
    }
    return s;
}
