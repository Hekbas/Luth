// ReSTIR GI shared types + helpers (Ouyang 2021). The GIReservoir layout mirrors the C++
// GPUGIReservoir in RtRestirGiSubsystem.cpp — any field change must update both. Distinct from the
// DI Reservoir (restir_common.glsl): a GI sample is a world-space path vertex, not a light index, so
// spatial/temporal reuse needs the reconnection Jacobian. see arch/rendering-pipeline.md

#ifndef LUTH_SHADERS_COMMON_RESTIR_GI
#define LUTH_SHADERS_COMMON_RESTIR_GI

#include "common/restir_common.glsl"   // PcgHash/RandFromSeed/Luminance/OctDecode + light structs

const float GI_PI = 3.14159265358979;

// 64 B, std430 — 4 × 16 B blocks. A 1-bounce path sample: secondary hit x_s, its normal n_s (oct),
// its outgoing radiance L_o toward the receiver, the reservoir state, and the receiver's own depth +
// normal self-carried for temporal validation + the temporal Jacobian's source-receiver term.
struct GIReservoir {
    vec3  samplePos;       float W;                 // x_s ; unbiased contribution weight (post-finalize)
    vec3  sampleRadiance;  float wSum;              // L_o (HDR, full float) ; running RIS weight sum
    vec2  sampleNormalOct; uint  M; uint age;       // n_s ; confidence ; frames survived
    vec2  visNormalOct;    float visDepth; float _pad; // receiver n_v (oct) + raw depth (self-carried)
};

void GIReservoirReset(out GIReservoir r) {
    r.samplePos = vec3(0.0); r.W = 0.0;
    r.sampleRadiance = vec3(0.0); r.wSum = 0.0;
    r.sampleNormalOct = vec2(0.0); r.M = 0u; r.age = 0u;
    r.visNormalOct = vec2(0.0); r.visDepth = 0.0; r._pad = 0.0;
}

// Octahedral encode — inverse of restir_common.glsl OctDecode (Cigolle et al. [0,1] mapping).
vec2 OctEncode(vec3 n) {
    n /= (abs(n.x) + abs(n.y) + abs(n.z));
    vec2 e = (n.z >= 0.0)
           ? n.xy
           : (vec2(1.0) - abs(n.yx)) * vec2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
    return e * 0.5 + 0.5;
}

// Stream one path-sample candidate into the reservoir (weighted reservoir sampling, Chao 1982).
void GIReservoirUpdate(inout GIReservoir r, vec3 xs, vec3 Lo, vec2 nsOct, float w, float rnd) {
    r.wSum += w;
    r.M    += 1u;
    if (r.wSum > 0.0 && rnd * r.wSum <= w) {
        r.samplePos       = xs;
        r.sampleRadiance  = Lo;
        r.sampleNormalOct = nsOct;
    }
}

// Merge a source reservoir's selected sample into c. risWeight = targetPdfAtCurr * Wsrc * Msrc, with
// the reconnection Jacobian already folded into targetPdfAtCurr (spatial) or Wsrc (temporal) by the
// caller. Confidence-weighted spatiotemporal combine (Ouyang 2021 / Bitterli 2020).
void GIReservoirMerge(inout GIReservoir c, vec3 xs, vec3 Lo, vec2 nsOct, uint ageSrc,
                      float targetPdfAtCurr, float Wsrc, uint Msrc, float rnd) {
    float w = targetPdfAtCurr * Wsrc * float(Msrc);
    c.wSum += w;
    c.M    += Msrc;
    if (c.wSum > 0.0 && rnd * c.wSum <= w) {
        c.samplePos       = xs;
        c.sampleRadiance  = Lo;
        c.sampleNormalOct = nsOct;
        c.age             = ageSrc;
    }
}

// Unbiased contribution weight for the selected sample. targetPdfAtSurface is the sample's target
// re-evaluated at the consuming pixel — Ouyang recomputes p-hat per surface (it is not stored).
void GIReservoirFinalize(inout GIReservoir r, float targetPdfAtSurface) {
    r.W = (targetPdfAtSurface > 0.0 && r.M > 0u) ? (r.wSum / (float(r.M) * targetPdfAtSurface)) : 0.0;
}

// GI target function — albedo-free, matching the demodulated output E = L_o * NdotL * W. The 1/dist^2
// and the cosine at x_s are NOT here — they ride in the reconnection Jacobian (double-count trap).
float GiTargetFunction(vec3 xs, vec3 Lo, vec3 receiverPos, vec3 receiverN) {
    vec3  L  = xs - receiverPos;
    float d2 = dot(L, L);
    if (d2 <= 0.0) return 0.0;
    L *= inversesqrt(d2);
    return Luminance(Lo) * max(dot(receiverN, L), 0.0);
}

// Pixar branchless orthonormal basis (Duff et al.) — TBN from a unit normal.
void BranchlessONB(vec3 n, out vec3 b1, out vec3 b2) {
    float s = (n.z >= 0.0) ? 1.0 : -1.0;
    float a = -1.0 / (s + n.z);
    float b = n.x * n.y * a;
    b1 = vec3(1.0 + s * n.x * n.x * a, s * b, -s * n.x);
    b2 = vec3(b, s + n.y * n.y * a, -n.y);
}

// Cosine-weighted hemisphere direction around n (local +Z = n). Returns a world-space unit dir and
// the solid-angle source pdf = cos(theta)/pi used as the initial reservoir weight (w = pHat / pdf).
vec3 SampleCosHemisphere(vec3 n, vec2 u, out float pdf) {
    vec3 t, bt; BranchlessONB(n, t, bt);
    float r   = sqrt(u.y);
    float phi = 2.0 * GI_PI * u.x;
    float z   = sqrt(max(0.0, 1.0 - u.y));   // = cos(theta)
    pdf = z / GI_PI;
    vec3 local = vec3(r * cos(phi), r * sin(phi), z);
    return normalize(t * local.x + bt * local.y + n * local.z);
}

// Reconnection-shift Jacobian (Ouyang 2021 Eq. 11): converts a reused sample's solid-angle measure
// from the source (neighbour/prev) receiver to the current receiver. CROSSED ratios — new cos / orig
// cos, orig dist^2 / new dist^2. BOTH cosines at the SAMPLE point (sampleNormal). Returns 0 on
// degenerate / back-facing reconnections (caller rejects via GiValidateJacobian).
float GiReconnectionJacobian(vec3 receiverNew, vec3 receiverOrig, vec3 samplePos, vec3 sampleNormal) {
    vec3  vN = receiverNew  - samplePos;
    vec3  vO = receiverOrig - samplePos;
    float dN = dot(vN, vN);
    float dO = dot(vO, vO);
    if (dN <= 0.0 || dO <= 0.0) return 0.0;
    float cosN = clamp(dot(sampleNormal, vN * inversesqrt(dN)), 0.0, 1.0);
    float cosO = clamp(dot(sampleNormal, vO * inversesqrt(dO)), 0.0, 1.0);
    float j = (cosN * dO) / (cosO * dN);
    return (isinf(j) || isnan(j)) ? 0.0 : j;
}

// Two-stage guard (RTXDI): reject geometrically incompatible reuse (J outside [1/10,10]); else clamp
// the survivor to [1/3,3] to bound fireflies. Returns false → drop the reused sample.
bool GiValidateJacobian(inout float j) {
    if (j > 10.0 || j < 0.1) return false;
    j = clamp(j, 1.0 / 3.0, 3.0);
    return true;
}

#endif
