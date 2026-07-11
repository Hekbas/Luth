// ReSTIR GI spatial reuse (Ouyang 2021) with RTXDI BASIC bias correction. Merges each pixel's
// temporal-output reservoir (b2, read-only) with a few disk neighbours into a SEPARATE spatial output (b6);
// neighbours read un-modified, never in-place. Each reused neighbour is reweighted by the reconnection
// Jacobian (Eq. 11). The final unbiased weight uses BASIC normalization (pi/piSum) instead of biased 1/M,
// then ONE final-visibility rayQuery (receiver -> selected sample) folds occlusion into W before the store:
// the output IS next frame's temporal history, so occluded samples stop persisting. AsyncCompute; shade
// reads b6. see arch/rendering-pipeline.md
import globals;
import restir_common;        // OctDecode / PcgHash / RandFromSeed
import restir_gi_common;     // GIReservoir + GiTargetFunction + Jacobian
import material;
import material_bindings_rt; // RtConfirmAlphaCandidates (Set 3 material, Set 4 bindless)

[[vk::binding(6, 0)]] RaytracingAccelerationStructure topLevelAS;
[[vk::binding(0, 2)]] Sampler2D u_SceneDepth;
[[vk::binding(1, 2)]] Sampler2D u_SlimNormal;
[[vk::binding(2, 2)]] StructuredBuffer<GIReservoir> inRes;        // temporal out
[[vk::binding(6, 2)]] RWStructuredBuffer<GIReservoir> outRes;     // spatial out

struct PC
{
    float4x4 invViewProj;
    uint     neighboursRadius;   // neighbours (low 16) | radius px (high 16)
    uint     frameSeed;
    float    depthThreshold;     // relative linear-depth tolerance
    float    normalThreshold;    // min dot(neighbourN, currN)
    int      gbufferScale;
    int      dispatchW;
    int      dispatchH;
    GtGeomEntry* geomTable;      // final-visibility alpha test; 8-aligned (C++ mirrors the 4 B pad)
};
[[vk::push_constant]] PC pc;

int2 GiGuide(int2 p)
{
    if (pc.gbufferScale == 1) return p;
    float2 uv   = (float2(p) + 0.5) / float2(pc.dispatchW, pc.dispatchH);
    int2   full = int2(ubo.viewportSize);
    return clamp(int2(uv * float2(full)), int2(0), full - int2(1));
}

static const uint GI_MAX_DOMAINS = 9u;   // canonical + up to 8 neighbours (BASIC denominator records)

float LinearizeDepth(float d) { return (ubo.nearZ * ubo.farZ) / (ubo.farZ - d * (ubo.farZ - ubo.nearZ)); }

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    int2 pixel = int2(tid.xy);
    int2 size  = int2(pc.dispatchW, pc.dispatchH);
    if (pixel.x >= size.x || pixel.y >= size.y) return;
    uint flatIdx = uint(pixel.y) * uint(size.x) + uint(pixel.x);

    float depth = u_SceneDepth.Load(int3(GiGuide(pixel), 0)).r;
    GIReservoir canonical = inRes[flatIdx];
    if (depth >= 1.0) { outRes[flatIdx] = canonical; return; }   // sky -> passthrough

    float2 octN     = u_SlimNormal.Load(int3(GiGuide(pixel), 0)).rg;
    float3 N        = OctDecode(octN);
    float2 uv       = (float2(pixel) + 0.5) / float2(size);
    float4 worldH   = mul(pc.invViewProj, float4(uv * 2.0 - 1.0, depth, 1.0));
    float3 worldPos = worldH.xyz / worldH.w;
    float  linDepth = LinearizeDepth(depth);

    const uint  neighbours = min(pc.neighboursRadius & 0xFFFFu, GI_MAX_DOMAINS - 1u);
    const float radius     = float(pc.neighboursRadius >> 16);

    uint seed = PcgHash(flatIdx ^ (pc.frameSeed * 2654435761u));

    // IGN-rotated Vogel disk (Jimenez 2014): blue-noise neighbour placement decorrelates the reuse set
    // between adjacent pixels (matches the DI spatial pass).
    uint   fo  = pc.frameSeed & 63u;
    float2 pf  = float2(pixel + int2(int(fo) * 5, int(fo) * 7));
    float  rot = frac(52.9829189 * frac(0.06711056 * pf.x + 0.00583715 * pf.y)) * 6.28318530718;

    // Per-domain surface records for the BASIC denominator (index 0 = canonical / this pixel).
    float3 domPos[GI_MAX_DOMAINS];
    float3 domN[GI_MAX_DOMAINS];
    uint   domM[GI_MAX_DOMAINS];
    uint   domCount = 0u;

    GIReservoir c; GIReservoirReset(c);
    int selDomain = -1;

    // Canonical (this pixel): identity shift, no Jacobian. ALWAYS seeds the reservoir's sample.
    {
        float pHat = GiTargetFunction(canonical.samplePos, canonical.sampleRadiance, worldPos, N);
        domPos[domCount] = worldPos; domN[domCount] = N; domM[domCount] = canonical.M;
        float w = pHat * canonical.W * float(canonical.M);
        c.wSum += w; c.M += canonical.M;
        RandFromSeed(seed);   // keep the neighbour RNG stream aligned with the prior conditional draw
        c.samplePos = canonical.samplePos; c.sampleRadiance = canonical.sampleRadiance;
        c.sampleNormalOct = canonical.sampleNormalOct; c.age = canonical.age;
        selDomain = int(domCount);
        domCount += 1u;
    }

    for (uint i = 0u; i < neighbours; ++i)
    {
        float ang = rot + (float(i) + 0.5) * 2.39996322973;   // golden-angle spiral
        float rad = radius * sqrt((float(i) + 0.5) / float(max(neighbours, 1u)));
        int2  q   = pixel + int2(int(round(cos(ang) * rad)), int(round(sin(ang) * rad)));
        if (q.x < 0 || q.y < 0 || q.x >= size.x || q.y >= size.y || (q.x == pixel.x && q.y == pixel.y)) continue;

        float qDepth = u_SceneDepth.Load(int3(GiGuide(q), 0)).r;
        if (qDepth >= 1.0) continue;
        float3 qN = OctDecode(u_SlimNormal.Load(int3(GiGuide(q), 0)).rg);
        if (dot(qN, N) < pc.normalThreshold) continue;                       // normal reject
        float qLin = LinearizeDepth(qDepth);
        if (abs(qLin - linDepth) > pc.depthThreshold * linDepth) continue;   // depth reject

        uint qFlat = uint(q.y) * uint(size.x) + uint(q.x);
        GIReservoir nr = inRes[qFlat];
        if (nr.M == 0u) continue;   // gate on confidence only (RTXDI): a zero-W neighbour still owes its M to piSum

        float2 qUv      = (float2(q) + 0.5) / float2(size);
        float4 qWh      = mul(pc.invViewProj, float4(qUv * 2.0 - 1.0, qDepth, 1.0));
        float3 qWorldPos = qWh.xyz / qWh.w;

        float3 nrSampleN = OctDecode(nr.sampleNormalOct);
        float  jac = GiReconnectionJacobian(worldPos, qWorldPos, nr.samplePos, nrSampleN);
        if (!GiValidateJacobian(jac)) continue;

        float pHat = GiTargetFunction(nr.samplePos, nr.sampleRadiance, worldPos, N);
        domPos[domCount] = qWorldPos; domN[domCount] = qN; domM[domCount] = nr.M;
        float w = (pHat * jac) * nr.W * float(nr.M);
        c.wSum += w; c.M += nr.M;
        if (c.wSum > 0.0 && RandFromSeed(seed) * c.wSum <= w)
        {
            c.samplePos = nr.samplePos; c.sampleRadiance = nr.sampleRadiance;
            c.sampleNormalOct = nr.sampleNormalOct; c.age = nr.age;
            selDomain = int(domCount);
        }
        domCount += 1u;
    }

    // RTXDI BASIC bias correction. W = wSum * selPi / (selTgt * piSum); piSum sums the SELECTED sample's
    // target re-evaluated at every contributing domain (x that domain's M). No accepted neighbours
    // collapses to canonical.W (passthrough).
    float selTgt = GiTargetFunction(c.samplePos, c.sampleRadiance, worldPos, N);
    float piSum  = 0.0;
    float selPi  = 0.0;
    for (uint d = 0u; d < domCount; ++d)
    {
        float ps = GiTargetFunction(c.samplePos, c.sampleRadiance, domPos[d], domN[d]);
        piSum += ps * float(domM[d]);
        if (int(d) == selDomain) selPi = ps;
    }
    c.W = (selTgt > 0.0 && piSum > 0.0 && c.M > 0u) ? (c.wSum * selPi / (selTgt * piSum)) : 0.0;

    // Final visibility receiver -> selected sample, folded MULTIPLICATIVELY into W before this reservoir
    // becomes next frame's history (never into the BASIC denominator: that breaks the partition of unity).
    // M is kept: occlusion is information, not invalidation; history self-cleans next frame.
    if (c.M > 0u && c.W > 0.0)
    {
        float3 toS    = c.samplePos - worldPos;
        float  dist   = length(toS);
        float3 d      = toS / max(dist, 1e-4);
        float  tMin   = ubo.rtShadowParams.y;
        float3 origin = worldPos + N * (ubo.rtShadowParams.z * 0.5) + d * tMin;
        RayDesc ray; ray.Origin = origin; ray.TMin = tMin; ray.Direction = d; ray.TMax = max(dist - 2.0 * tMin, tMin);
        RayQuery<RAY_FLAG_NONE> rq;
        rq.TraceRayInline(topLevelAS, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH, GT_VIS_SOLID, ray);
        RtConfirmAlphaCandidates(rq, pc.geomTable);
        if (rq.CommittedStatus() != COMMITTED_NOTHING) c.W = 0.0;
    }

    c.visPos          = worldPos;
    c.visNormalPacked = f32tof16(octN.x) | (f32tof16(octN.y) << 16u);
    outRes[flatIdx] = c;
}
