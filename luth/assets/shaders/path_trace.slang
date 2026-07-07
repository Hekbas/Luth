// Path-traced reference mode: megakernel. Traces its own jittered primary rays (free
// AA on convergence), then a multi-bounce NEE walk with cosine/GGX-VNDF BSDF sampling, Russian-roulette
// termination, emission + environment-on-miss, progressively accumulated to an fp32 running mean.
// see arch/rendering-pipeline.md
import globals;
import restir_common;       // light structs + PCG/luminance + PointLightRadiance
import restir_gi_common;    // BranchlessONB + SampleCosHemisphere + GI_PI
import brdf;                // Cook-Torrance + GGX-VNDF sampling + lobe pdfs
import material;
import material_bindings_rt; // FetchHitSurface + RtConfirmAlphaCandidates (Set 3 material, Set 4 bindless)

[[vk::binding(2, 0)]] SamplerCube u_PrefilteredEnv;   // IBL prefiltered env (LOD 0 ~ sky radiance)
[[vk::binding(6, 0)]] RaytracingAccelerationStructure topLevelAS;
[[vk::binding(0, 1)]] ByteAddressBuffer g_Lights;
[[vk::binding(0, 2)]] [[vk::image_format("rgba32f")]] RWTexture2D<float4> u_Accum;   // fp32 progressive mean
[[vk::binding(1, 2)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> u_Color;   // display copy

struct PC
{
    float4x4     invViewProj;
    uint         frameSeed;
    uint         sampleCount;       // paths accumulated BEFORE this frame
    uint         samplesPerFrame;
    uint         maxBounces;
    uint         rrStartDepth;
    uint         reset;             // 1 -> ignore the accumulator this frame
    float        fireflyClamp;      // per-bounce indirect clamp
    float        pad0;
    uint         envReady;          // 1 -> IBL prefiltered env bound
    uint         pad1;
    GtGeomEntry* geomTable;
};
[[vk::push_constant]] PC pc;

// LightBuffer header = DirectionalLightData(32) + count(4) + pad[3](12); points[] (32 B each) at offset 48.
DirectionalLightData LoadDirLight()
{
    DirectionalLightData d;
    d.direction = asfloat(g_Lights.Load3(0));
    d.intensity = asfloat(g_Lights.Load(12));
    d.color     = asfloat(g_Lights.Load3(16));
    d._pad      = 0.0;
    return d;
}
uint LoadPointCount() { return g_Lights.Load(32); }
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

float3 SampleEnv(float3 dir)
{
    if (pc.envReady == 0u) return float3(0.0);
    return u_PrefilteredEnv.SampleLevel(dir, 0.0).rgb * ubo.skyboxIntensity;
}

// Shadow ray: true iff `dir` is unoccluded from x up to maxDist (Wächter-Binder geometric-normal offset).
bool Visible(float3 x, float3 offsetN, float3 dir, float maxDist)
{
    float  sMin  = ubo.rtShadowParams.y;
    float3 sOrig = x + offsetN * (ubo.rtShadowParams.z * 0.5) + dir * sMin;
    RayDesc ray; ray.Origin = sOrig; ray.TMin = sMin; ray.Direction = dir; ray.TMax = max(maxDist, sMin);
    RayQuery<RAY_FLAG_NONE> sq;
    sq.TraceRayInline(topLevelAS, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH, GT_VIS_SOLID, ray);
    RtConfirmAlphaCandidates(sq, pc.geomTable);
    return sq.CommittedStatus() == COMMITTED_NOTHING;
}

// Full-BRDF NEE at (x, surface s) with view dir V. One random point light (x count undoes the 1/count
// pick pdf) + deterministic sun, each shadow-rayed.
float3 DirectLighting(float3 x, float3 V, HitSurface s, inout uint seed)
{
    float3 Lo = float3(0.0);
    uint lc = LoadPointCount();
    if (lc > 0u)
    {
        uint j = min(uint(RandFromSeed(seed) * float(lc)), lc - 1u);
        PointLightData pl = LoadPoint(j);
        float3 Lw; float ld;
        float3 Li = PointLightRadiance(pl, x, Lw, ld);
        if (dot(s.ns, Lw) > 0.0 && Visible(x, s.ng, Lw, ld - 2.0 * ubo.rtShadowParams.y))
            Lo += EvalBRDFTimesNdotL(Lw, Li * float(lc), V, s.ns, s.baseColor, s.metalness, s.roughness);
    }
    DirectionalLightData dl = LoadDirLight();
    if (dl.intensity > 0.0)
    {
        float3 Lsun = normalize(-dl.direction);
        if (dot(s.ns, Lsun) > 0.0 && Visible(x, s.ng, Lsun, 1.0e30))
            Lo += EvalBRDFTimesNdotL(Lsun, dl.color * dl.intensity, V, s.ns, s.baseColor, s.metalness, s.roughness);
    }
    return Lo;
}

// BSDF importance sampling: pick diffuse or specular lobe, sample it, weight by one-sample lobe MIS.
// Returns the next ray direction; multiplies `tp` by f*cos/pdf. false -> dead path.
bool SampleBSDF(float3 V, HitSurface s, inout uint seed, out float3 newDir, inout float3 tp)
{
    float3 F0    = lerp(float3(0.04), s.baseColor, s.metalness);
    float  wDiff = Luminance((1.0 - s.metalness) * s.baseColor);
    float  wSpec = Luminance(F0);
    // One-sample MIS is unbiased for any p in (0,1); the [0.1,0.9] clamp only trades variance to never
    // starve the rare-but-bright specular highlight (or a metal's diffuse 10%).
    float pSpec = clamp(wSpec / max(wDiff + wSpec, 1.0e-4), 0.1, 0.9);

    float3 t, bt; BranchlessONB(s.ns, t, bt);
    if (RandFromSeed(seed) < pSpec)
    {
        float3 Vt = float3(dot(V, t), dot(V, bt), dot(V, s.ns));
        float ua = RandFromSeed(seed); float ub = RandFromSeed(seed);
        float3 Ht = SampleGGXVNDF(Vt, s.roughness * s.roughness, ua, ub);
        float3 H  = Ht.x * t + Ht.y * bt + Ht.z * s.ns;
        newDir    = reflect(-V, H);
    }
    else
    {
        float ua = RandFromSeed(seed); float ub = RandFromSeed(seed);
        float pdf;
        newDir = SampleCosHemisphere(s.ns, float2(ua, ub), pdf);
    }

    float NdotL = dot(s.ns, newDir);
    if (NdotL <= 0.0) return false;   // below the surface: terminate

    float3 fCos = EvalBRDFTimesNdotL(newDir, float3(1.0), V, s.ns, s.baseColor, s.metalness, s.roughness);
    float  pdf  = pSpec * PdfSpec(s.ns, V, newDir, s.roughness) + (1.0 - pSpec) * PdfDiffuse(s.ns, newDir);
    if (pdf <= 0.0) return false;
    tp *= fCos / pdf;
    return true;
}

// One full sample path: jittered primary ray, then a multi-bounce NEE walk (punctual via NEE only,
// emissive via BSDF hit only, environment via miss only; disjoint, so no cross-technique MIS needed).
float3 SamplePath(int2 pixel, int2 size, inout uint seed)
{
    float jx = RandFromSeed(seed); float jy = RandFromSeed(seed);
    float2 uv  = (float2(pixel) + 0.5 + (float2(jx, jy) - 0.5)) / float2(size);
    float2 ndc = uv * 2.0 - 1.0;
    float4 farH = mul(pc.invViewProj, float4(ndc, 1.0, 1.0));
    float3 farP = farH.xyz / farH.w;
    float3 ro   = ubo.cameraPos;
    float3 rd   = normalize(farP - ro);

    float  tMin       = ubo.rtShadowParams.y;
    float3 radiance   = float3(0.0);
    float3 throughput = float3(1.0);

    for (uint bounce = 0u; bounce < pc.maxBounces; ++bounce)
    {
        RayDesc ray; ray.Origin = ro; ray.TMin = tMin; ray.Direction = rd; ray.TMax = 1.0e30;
        RayQuery<RAY_FLAG_NONE> rq;
        rq.TraceRayInline(topLevelAS, RAY_FLAG_NONE, GT_VIS_ALL, ray);
        RtConfirmAlphaCandidates(rq, pc.geomTable);
        if (rq.CommittedStatus() == COMMITTED_NOTHING)
        {
            radiance += throughput * SampleEnv(rd);
            break;
        }

        float  t = rq.CommittedRayT();
        float3 x = ro + rd * t;
        HitSurface s = FetchHitSurface(pc.geomTable, rq.CommittedInstanceID(), rq.CommittedPrimitiveIndex(),
                                       rq.CommittedTriangleBarycentrics(), rq.CommittedObjectToWorld3x4(), rd, x);

        float3 V = -rd;
        float3 contrib = throughput * (s.emission + DirectLighting(x, V, s, seed));
        // Firefly clamp INDIRECT contributions only (primary direct/emission stays unbiased).
        if (bounce > 0u)
        {
            float lum = Luminance(contrib);
            if (lum > pc.fireflyClamp) contrib *= pc.fireflyClamp / lum;
        }
        radiance += contrib;

        float3 newDir;
        if (!SampleBSDF(V, s, seed, newDir, throughput)) break;

        // Russian roulette after a few guaranteed bounces; /survival keeps the estimator unbiased.
        if (bounce >= pc.rrStartDepth)
        {
            float p = clamp(max(throughput.r, max(throughput.g, throughput.b)), 0.05, 1.0);
            if (RandFromSeed(seed) > p) break;
            throughput /= p;
        }

        ro = x + s.ng * (ubo.rtShadowParams.z * 0.5) + newDir * tMin;
        rd = newDir;
    }
    return radiance;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    int2 pixel = int2(tid.xy);
    int2 size  = int2(ubo.viewportSize);
    if (pixel.x >= size.x || pixel.y >= size.y) return;
    uint flatIdx = uint(pixel.y) * uint(size.x) + uint(pixel.x);

    uint seed = PcgHash(flatIdx ^ (pc.frameSeed * 2654435761u));
    uint spp  = max(pc.samplesPerFrame, 1u);

    float3 sum = float3(0.0);
    for (uint i = 0u; i < spp; ++i)
        sum += SamplePath(pixel, size, seed);

    // Unbiased running mean. Reset / first frame: this frame seeds; else blend by accumulated count.
    float3 result;
    if (pc.reset != 0u || pc.sampleCount == 0u)
    {
        result = sum / float(spp);
    }
    else
    {
        float3 old = u_Accum[uint2(pixel)].rgb;
        float  n   = float(pc.sampleCount);
        result = (old * n + sum) / (n + float(spp));
    }
    u_Accum[uint2(pixel)] = float4(result, 0.0);
    u_Color[uint2(pixel)] = float4(result, 1.0);
}
