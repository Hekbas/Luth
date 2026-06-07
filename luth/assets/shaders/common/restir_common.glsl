// ReSTIR DI shared types + helpers (Bitterli 2020). The Reservoir layout mirrors the C++
// GPUReservoir in RtRestirSubsystem.cpp — any field change must update both. see arch/rendering-pipeline.md

#ifndef LUTH_SHADERS_COMMON_RESTIR
#define LUTH_SHADERS_COMMON_RESTIR

struct DirectionalLightData {
    vec3  direction;
    float intensity;
    vec3  color;
    float _pad;
};

struct PointLightData {
    vec3  position;
    float range;
    vec3  color;
    float intensity;
};

// 32 B, std430. _pad slots reserve a future point-on-light / uv for area lights + ReSTIR GI.
struct Reservoir {
    uint  lightIndex;   // index into lights.points[]; RESTIR_NO_LIGHT when empty
    float W;            // unbiased contribution weight (Bitterli 2020)
    float wSum;         // running sum of resampling weights
    uint  M;            // sample count / confidence
    float targetPdf;    // p-hat of the selected sample
    float _pad0;
    float _pad1;
    float _pad2;
};

const uint RESTIR_NO_LIGHT = 0xFFFFFFFFu;

void ReservoirReset(out Reservoir r) {
    r.lightIndex = RESTIR_NO_LIGHT;
    r.W = 0.0; r.wSum = 0.0; r.M = 0u; r.targetPdf = 0.0;
    r._pad0 = 0.0; r._pad1 = 0.0; r._pad2 = 0.0;
}

// Stream one candidate into the reservoir (weighted reservoir sampling, Chao 1982).
void ReservoirUpdate(inout Reservoir r, uint lightIndex, float targetPdf, float w, float rnd) {
    r.wSum += w;
    r.M    += 1u;
    if (r.wSum > 0.0 && rnd * r.wSum <= w) {
        r.lightIndex = lightIndex;
        r.targetPdf  = targetPdf;
    }
}

// Unbiased contribution weight for the selected sample.
void ReservoirFinalize(inout Reservoir r) {
    r.W = (r.targetPdf > 0.0 && r.M > 0u) ? (r.wSum / (float(r.M) * r.targetPdf)) : 0.0;
}

// Merge a source reservoir's selected sample into c, weighted by its resampling weight
// (targetPdf-at-this-pixel * W * M). Confidence-weighted spatiotemporal combine (Bitterli 2020) —
// targetPdfAtCurr must be re-evaluated at the consuming pixel for reused (prev/neighbor) samples.
void ReservoirMerge(inout Reservoir c, uint lightIndex, float targetPdfAtCurr, float Wsrc, uint Msrc, float rnd) {
    float w = targetPdfAtCurr * Wsrc * float(Msrc);
    c.wSum += w;
    c.M    += Msrc;
    if (c.wSum > 0.0 && rnd * c.wSum <= w) {
        c.lightIndex = lightIndex;
        c.targetPdf  = targetPdfAtCurr;
    }
}

float Luminance(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }

// PCG hash → uint; RandFromSeed advances the stream and returns [0,1).
uint PcgHash(uint v) {
    uint state = v * 747796405u + 2891336453u;
    uint word  = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}
float RandFromSeed(inout uint seed) {
    seed = PcgHash(seed);
    return float(seed) * (1.0 / 4294967296.0);
}

// Octahedral decode — inverse of slim_gbuffer.frag OctEncode.
vec3 OctDecode(vec2 enc) {
    enc = enc * 2.0 - 1.0;
    vec3 n = vec3(enc.xy, 1.0 - abs(enc.x) - abs(enc.y));
    float t = max(-n.z, 0.0);
    n.x += (n.x >= 0.0) ? -t : t;
    n.y += (n.y >= 0.0) ? -t : t;
    return normalize(n);
}

// Incoming radiance from a point light at worldPos — matches pbr.frag attenuation + rolloff.
vec3 PointLightRadiance(PointLightData pl, vec3 worldPos, out vec3 L, out float dist) {
    vec3 toLight = pl.position - worldPos;
    dist = length(toLight);
    L    = (dist > 0.0) ? toLight / dist : vec3(0.0, 1.0, 0.0);
    float atten   = 1.0 / max(dist * dist, 1e-4);
    float rolloff = pow(clamp(1.0 - dist / max(pl.range, 1e-4), 0.0, 1.0), 2.0);
    return pl.color * pl.intensity * atten * rolloff;
}

#endif
