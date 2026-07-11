// ReSTIR DI spatial reuse (Bitterli 2020). Each pixel merges its own reservoir with a few random
// neighbours inside a disk, rejecting geometrically dissimilar ones (linear depth + normal). Punctual
// point lights store a light INDEX, so the shift map is identity and there is NO Jacobian: the
// neighbour's light target is re-evaluated at this pixel. Reads the temporal output (in) and
// writes a separate output (out); neighbours must be read un-modified, so this can't be in-place.
// The output IS next frame's temporal history: one final-visibility rayQuery folds occlusion into W
// before the store (occluded samples stop persisting), and the pixel's depth + oct normal are
// self-carried in the pads for next frame's validation. A groupwide boiling filter then resets
// outlier-W reservoirs before the store (main has NO early returns: its barriers need every thread).
// AsyncCompute. see arch/rendering-pipeline.md
import globals;
import restir_common;
import restir_di_target;    // RestirTargetPdf
import material;
import material_bindings_rt; // RtConfirmAlphaCandidates (Set 3 material, Set 4 bindless)

[[vk::binding(6, 0)]] RaytracingAccelerationStructure topLevelAS;
[[vk::binding(0, 1)]] ByteAddressBuffer g_Lights;
[[vk::binding(0, 2)]] Sampler2D u_Depth;
[[vk::binding(1, 2)]] Sampler2D u_Normal;
[[vk::binding(2, 2)]] StructuredBuffer<Reservoir> resIn;
[[vk::binding(6, 2)]] RWStructuredBuffer<Reservoir> resOut;
[[vk::binding(7, 2)]] Sampler2D u_SlimRoughness;   // combined diffuse+spec target

struct PC
{
    float4x4 invViewProj;
    uint     neighbourCount;
    uint     radius;          // disk radius in pixels
    uint     frameSeed;
    float    depthThreshold;  // relative linear-depth tolerance
    int      gbufferScale;
    int      dispatchW;
    int      dispatchH;
    float    normalThreshold;     // min dot(neighbourN, currN)
    float    roughnessThreshold;  // max |neighbourRough - rough| (spec reuse gate)
    float    boilingStrength;     // boiling-filter kill knob, 0 disables (fills the pre-pointer pad)
    GtGeomEntry* geomTable;       // final-visibility alpha test; stays 8-aligned at offset 104
};
[[vk::push_constant]] PC pc;

PointLightData LoadPoint(uint j)
{
    uint b = 48u + j * 32u;
    PointLightData p;
    p.position  = asfloat(g_Lights.Load3(b + 0u));
    p.range     = asfloat(g_Lights.Load(b + 12u));
    p.color     = asfloat(g_Lights.Load3(b + 16u));
    p.intensity = asfloat(g_Lights.Load(b + 28u));
    return p;
}

int2 GbufCoord(int2 p)
{
    if (pc.gbufferScale == 1) return p;
    float2 uv   = (float2(p) + 0.5) / float2(pc.dispatchW, pc.dispatchH);
    int2   full = int2(ubo.viewportSize);
    return clamp(int2(uv * float2(full)), int2(0), full - int2(1));
}

float LinearizeDepth(float d) { return (ubo.nearZ * ubo.farZ) / (ubo.farZ - d * (ubo.farZ - ubo.nearZ)); }

static const uint DI_MAX_DOMAINS = 9u;   // canonical + up to 8 neighbours (BASIC denominator records)

// Boiling filter (RTXDI): average the NONZERO reservoir weights across the 8x8 threadgroup and kill any
// reservoir whose weight exceeds avg * mult, mult = 10/strength - 9 (strength 0.2 -> ~41x). One rare
// over-bright reservoir otherwise persists as history and spreads through reuse (the boiling blobs).
// invariant: EVERY thread of the group calls this exactly once (group barriers inside); dead threads
// (out-of-bounds / sky) pass weight 0 so they neither shift the average nor get killed.
static const uint BOIL_GROUP = 64u;   // must match [numthreads] (8 * 8)
groupshared float gs_BoilWeight[BOIL_GROUP];
groupshared uint  gs_BoilCount[BOIL_GROUP];

bool BoilingKillsReservoir(uint gtIdx, float weight)
{
    gs_BoilWeight[gtIdx] = weight;
    gs_BoilCount[gtIdx]  = (weight > 0.0) ? 1u : 0u;
    GroupMemoryBarrierWithGroupSync();
    for (uint s = BOIL_GROUP / 2u; s > 0u; s >>= 1u)
    {
        if (gtIdx < s)
        {
            gs_BoilWeight[gtIdx] += gs_BoilWeight[gtIdx + s];
            gs_BoilCount[gtIdx]  += gs_BoilCount[gtIdx + s];
        }
        GroupMemoryBarrierWithGroupSync();
    }
    if (pc.boilingStrength <= 0.0 || gs_BoilCount[0] == 0u) return false;
    float avg  = gs_BoilWeight[0] / float(gs_BoilCount[0]);
    float mult = 10.0 / clamp(pc.boilingStrength, 1e-6, 1.0) - 9.0;
    return weight > avg * mult;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID, uint gtIdx : SV_GroupIndex)
{
    int2 pixel = int2(tid.xy);
    int2 size  = int2(pc.dispatchW, pc.dispatchH);

    // No early returns past here: BoilingKillsReservoir's barriers need the whole group. Out-of-bounds
    // and sky threads stay 'dead' (skip the resampling work, stream weight 0) but reach the barriers.
    bool inBounds = pixel.x < size.x && pixel.y < size.y;
    uint flatIdx  = inBounds ? uint(pixel.y) * uint(size.x) + uint(pixel.x) : 0u;

    Reservoir canonical; ReservoirReset(canonical);
    float depth = 1.0;
    if (inBounds)
    {
        canonical = resIn[flatIdx];
        depth     = u_Depth.Load(int3(GbufCoord(pixel), 0)).r;
    }
    bool alive = inBounds && depth < 1.0;   // sky passes canonical through unfiltered

    Reservoir c; ReservoirReset(c);
    float2 octN = float2(0.0);
    if (alive)
    {
        octN = u_Normal.Load(int3(GbufCoord(pixel), 0)).rg;
        float3 N        = OctDecode(octN);
        float2 uv       = (float2(pixel) + 0.5) / float2(size);
        float2 uvR      = DejitterReconUV(uv, ubo.taaParams.zw);   // un-jittered recon; see globals
        float4 worldH   = mul(pc.invViewProj, float4(uvR * 2.0 - 1.0, depth, 1.0));
        float3 worldPos = worldH.xyz / worldH.w;
        float  currLin  = LinearizeDepth(depth);
        float3 V        = normalize(ubo.cameraPos - worldPos);
        float  rough    = u_SlimRoughness.Load(int3(GbufCoord(pixel), 0)).r;

        const uint neighbours = min(pc.neighbourCount, DI_MAX_DOMAINS - 1u);
        uint seed = PcgHash(flatIdx ^ (pc.frameSeed * 2246822519u));

        // Per-domain surface records for the BASIC denominator (index 0 = canonical / this pixel).
        float3 domPos[DI_MAX_DOMAINS];
        float3 domN[DI_MAX_DOMAINS];
        float  domRough[DI_MAX_DOMAINS];
        uint   domM[DI_MAX_DOMAINS];
        uint   domCount = 0u;

        int selDomain = -1;

        // Canonical (this pixel): identity shift, no Jacobian. Records + seeds domain 0.
        domPos[0] = worldPos; domN[0] = N; domRough[0] = rough; domM[0] = canonical.M;
        if (ReservoirMergeSel(c, canonical.lightIndex, canonical.targetPdf, canonical.W, canonical.M, RandFromSeed(seed)))
            selDomain = 0;
        domCount = 1u;

        // Low-discrepancy Vogel disk rotated per pixel+frame (IGN, Jimenez 2014): decorrelates the reuse set
        // between adjacent pixels, so the spatial noise the denoiser sees is blue, not the clumpy white the RNG
        // gave (the audited "structured spatial-reuse patterns").
        uint   fo  = pc.frameSeed & 63u;
        float2 pf  = float2(pixel + int2(int(fo) * 5, int(fo) * 7));
        float  rot = frac(52.9829189 * frac(0.06711056 * pf.x + 0.00583715 * pf.y)) * 6.28318530718;

        for (uint i = 0u; i < neighbours; ++i)
        {
            float ang = rot + (float(i) + 0.5) * 2.39996322973;                              // golden-angle spiral
            float rad = float(pc.radius) * sqrt((float(i) + 0.5) / float(max(neighbours, 1u)));
            int2  np  = pixel + int2(int(round(cos(ang) * rad)), int(round(sin(ang) * rad)));
            if (np.x < 0 || np.y < 0 || np.x >= size.x || np.y >= size.y || all(np == pixel)) continue;

            float nd = u_Depth.Load(int3(GbufCoord(np), 0)).r;
            if (nd >= 1.0) continue;
            float3 nN = OctDecode(u_Normal.Load(int3(GbufCoord(np), 0)).rg);
            if (dot(nN, N) < pc.normalThreshold) continue;                                    // normal reject
            if (abs(currLin - LinearizeDepth(nd)) > pc.depthThreshold * currLin) continue;    // depth reject
            float nRough = u_SlimRoughness.Load(int3(GbufCoord(np), 0)).r;
            if (abs(nRough - rough) > pc.roughnessThreshold) continue;                        // roughness reject (spec)

            Reservoir nr = resIn[uint(np.y) * uint(size.x) + uint(np.x)];
            if (nr.M == 0u) continue;   // gate on confidence only (RTXDI): a zero-W neighbour still owes M to piSum

            float2 nUv       = (float2(np) + 0.5) / float2(size);
            float2 nUvR      = DejitterReconUV(nUv, ubo.taaParams.zw);   // match the canonical de-jitter
            float4 nWh       = mul(pc.invViewProj, float4(nUvR * 2.0 - 1.0, nd, 1.0));
            float3 nWorldPos = nWh.xyz / nWh.w;

            // Streaming weight: neighbour's own light re-evaluated at THIS pixel (punctual -> no Jacobian). A
            // NO_LIGHT (fully-occluded) neighbour streams w=0 but still records its domain for the denominator.
            float pHat = 0.0;
            if (nr.lightIndex != RESTIR_NO_LIGHT)
            {
                PointLightData pl = LoadPoint(nr.lightIndex);
                float3 L; float dist;
                float3 Li = PointLightRadiance(pl, worldPos, L, dist);
                pHat = RestirTargetPdf(N, V, L, Li, rough);
            }
            domPos[domCount] = nWorldPos; domN[domCount] = nN; domRough[domCount] = nRough; domM[domCount] = nr.M;
            if (ReservoirMergeSel(c, nr.lightIndex, pHat, nr.W, nr.M, RandFromSeed(seed))) selDomain = int(domCount);
            domCount += 1u;
        }

        // RTXDI BASIC: W = wSum * selPi / (selTgt * piSum). piSum re-evaluates the WINNER's light at every
        // contributing domain (x its M), so a domain whose target vanishes at the winner drops out of the
        // denominator instead of darkening the estimate. No accepted neighbour -> passthrough (canonical.W).
        float Wout = 0.0;
        if (c.lightIndex != RESTIR_NO_LIGHT && c.M > 0u)
        {
            PointLightData sel = LoadPoint(c.lightIndex);
            float3 Ls; float ds;
            float3 Lis    = PointLightRadiance(sel, worldPos, Ls, ds);
            float  selTgt = RestirTargetPdf(N, V, Ls, Lis, rough);
            float  piSum  = 0.0;
            float  selPi  = 0.0;
            for (uint d = 0u; d < domCount; ++d)
            {
                float3 Ld; float dd;
                float3 LiD = PointLightRadiance(sel, domPos[d], Ld, dd);
                float3 Vd  = normalize(ubo.cameraPos - domPos[d]);
                float  ps  = RestirTargetPdf(domN[d], Vd, Ld, LiD, domRough[d]);
                piSum += ps * float(domM[d]);
                if (int(d) == selDomain) selPi = ps;
            }
            Wout = (selTgt > 0.0 && piSum > 0.0) ? (c.wSum * selPi / (selTgt * piSum)) : 0.0;

            // Final visibility of the selected sample, folded MULTIPLICATIVELY into W before this reservoir
            // becomes next frame's history (never into the BASIC denominator: that breaks the partition of
            // unity). M is kept: occlusion is information, not invalidation; history self-cleans next frame.
            // Same ray recipe as restir_initial (Ls/ds from PointLightRadiance above).
            if (Wout > 0.0)
            {
                float  tMin   = ubo.rtShadowParams.y;
                float3 origin = worldPos + N * (ubo.rtShadowParams.z * 0.5) + Ls * tMin;
                RayDesc ray; ray.Origin = origin; ray.TMin = tMin; ray.Direction = Ls; ray.TMax = max(ds - 2.0 * tMin, tMin);
                RayQuery<RAY_FLAG_NONE> rq;
                rq.TraceRayInline(topLevelAS, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH, GT_VIS_SOLID, ray);
                RtConfirmAlphaCandidates(rq, pc.geomTable);
                if (rq.CommittedStatus() != COMMITTED_NOTHING) Wout = 0.0;
            }
        }
        c.W = Wout;
    }

    // Boiling filter over the final (post-visibility) W: what survives here is what persists as history.
    if (BoilingKillsReservoir(gtIdx, alive ? c.W : 0.0)) ReservoirReset(c);

    if (alive)
    {
        // Self-carry this pixel's geometry (raw depth + oct normal) for next frame's temporal validation:
        // this buffer IS the history now, so the carry lives here, not in the temporal pass. Written on a
        // boiling reset too (harmless: temporal gates history on M > 0 before reading the pads).
        c._pad0 = depth;
        c._pad1 = octN.x;
        c._pad2 = octN.y;
        resOut[flatIdx] = c;
    }
    else if (inBounds)
        resOut[flatIdx] = canonical;   // sky: pass the temporal reservoir through untouched
}
