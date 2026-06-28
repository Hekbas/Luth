// ReSTIR DI shared types + helpers (Bitterli 2020) — twin of restir_common.glsl (the GLSL DI pipeline
// still uses that). Reservoir layout mirrors the C++ GPUReservoir. see arch/rendering-pipeline.md

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

// 32 B, std430. _pad slots reserve a future point-on-light / uv for area lights + ReSTIR GI.
public struct Reservoir
{
    public uint  lightIndex;   // index into points[]; RESTIR_NO_LIGHT when empty
    public float W;            // unbiased contribution weight (Bitterli 2020)
    public float wSum;         // running sum of resampling weights
    public uint  M;            // sample count / confidence
    public float targetPdf;    // p-hat of the selected sample
    public float _pad0, _pad1, _pad2;
}

public static const uint RESTIR_NO_LIGHT = 0xFFFFFFFFu;

public void ReservoirReset(out Reservoir r)
{
    r.lightIndex = RESTIR_NO_LIGHT;
    r.W = 0.0; r.wSum = 0.0; r.M = 0u; r.targetPdf = 0.0;
    r._pad0 = 0.0; r._pad1 = 0.0; r._pad2 = 0.0;
}

// Stream one candidate into the reservoir (weighted reservoir sampling, Chao 1982).
public void ReservoirUpdate(inout Reservoir r, uint lightIndex, float targetPdf, float w, float rnd)
{
    r.wSum += w;
    r.M    += 1u;
    if (r.wSum > 0.0 && rnd * r.wSum <= w) { r.lightIndex = lightIndex; r.targetPdf = targetPdf; }
}

public void ReservoirFinalize(inout Reservoir r)
{
    r.W = (r.targetPdf > 0.0 && r.M > 0u) ? (r.wSum / (float(r.M) * r.targetPdf)) : 0.0;
}

// Confidence-weighted spatiotemporal combine (Bitterli 2020); targetPdfAtCurr re-evaluated per pixel.
public void ReservoirMerge(inout Reservoir c, uint lightIndex, float targetPdfAtCurr, float Wsrc, uint Msrc, float rnd)
{
    float w = targetPdfAtCurr * Wsrc * float(Msrc);
    c.wSum += w;
    c.M    += Msrc;
    if (c.wSum > 0.0 && rnd * c.wSum <= w) { c.lightIndex = lightIndex; c.targetPdf = targetPdfAtCurr; }
}

public float Luminance(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }

// PCG hash → uint; RandFromSeed advances the stream and returns [0,1).
public uint PcgHash(uint v)
{
    uint state = v * 747796405u + 2891336453u;
    uint word  = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}
public float RandFromSeed(inout uint seed) { seed = PcgHash(seed); return float(seed) * (1.0 / 4294967296.0); }

// Octahedral decode — inverse of slim_gbuffer.frag OctEncode.
public float3 OctDecode(float2 enc)
{
    enc = enc * 2.0 - 1.0;
    float3 n = float3(enc.xy, 1.0 - abs(enc.x) - abs(enc.y));
    float t = max(-n.z, 0.0);
    n.x += (n.x >= 0.0) ? -t : t;
    n.y += (n.y >= 0.0) ? -t : t;
    return normalize(n);
}

// Incoming radiance from a point light at worldPos — matches the raster attenuation + rolloff.
public float3 PointLightRadiance(PointLightData pl, float3 worldPos, out float3 L, out float dist)
{
    float3 toLight = pl.position - worldPos;
    dist = length(toLight);
    L    = (dist > 0.0) ? toLight / dist : float3(0.0, 1.0, 0.0);
    float atten   = 1.0 / max(dist * dist, 1e-4);
    float rolloff = pow(clamp(1.0 - dist / max(pl.range, 1e-4), 0.0, 1.0), 2.0);
    return pl.color * pl.intensity * atten * rolloff;
}
