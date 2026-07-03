// ReSTIR GI initial 1-bounce path-sample generation (Ouyang 2021). Per pixel: reconstruct the primary
// hit x_v, cosine-sample one hemisphere direction, trace to the secondary hit x_s, shade its outgoing
// radiance L_o toward x_v (single-light NEE), store a GI reservoir (M=1). Reconnection Jacobian is applied
// later (temporal/spatial reuse), never here. AsyncCompute. see arch/rendering-pipeline.md
import globals;
import restir_common;
import restir_gi_common;
import material;
import material_bindings_rt;   // FetchHitSurface + RtConfirmAlphaCandidates (Set 3 material, Set 4 bindless)

static const bool GI_USE_SCAFFOLD_LO = false;   // debug: constant albedo + receiver-facing normal

// Set 0 b6: TLAS (b0 = GlobalUniforms via globals). Set 1 b0: LightSSBO, header + flexible point array,
// byte-loaded to dodge std430 struct-layout drift. Set 2: scene depth + slim normal + GI reservoir (write).
[[vk::binding(6, 0)]] RaytracingAccelerationStructure topLevelAS;
[[vk::binding(0, 1)]] ByteAddressBuffer g_Lights;
[[vk::binding(0, 2)]] Sampler2D u_SceneDepth;
[[vk::binding(1, 2)]] Sampler2D u_SlimNormal;     // RG16F oct
[[vk::binding(2, 2)]] RWStructuredBuffer<GIReservoir> reservoirs;

struct PC
{
    float4x4     invViewProj;
    uint         frameSeed;
    float        secondaryAlbedo;   // scaffold constant only
    float        maxIndirect;       // firefly clamp on L_o luminance
    int          gbufferScale;      // 1 = full-res; 2 = half-res GI (G-buffer reads remap to full)
    int          dispatchW;         // GI working (dispatch) resolution
    int          dispatchH;
    GtGeomEntry* geomTable;         // 64-bit handle; 0 before the first real TLAS build
};
[[vk::push_constant]] PC pc;

// Map a working-res pixel to the full-res G-buffer texel (depth/normal live at full res).
int2 GiGuide(int2 p)
{
    if (pc.gbufferScale == 1) return p;
    float2 uv   = (float2(p) + 0.5) / float2(pc.dispatchW, pc.dispatchH);
    int2   full = int2(ubo.viewportSize);
    return clamp(int2(uv * float2(full)), int2(0), full - int2(1));
}

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

// 1 = blocked. SOLID mask so glass never blocks light (geom-table contract); alpha-tested cutout holes.
float TraceVisibility(float3 origin, float3 dir, float tMin, float tMax)
{
    RayDesc ray; ray.Origin = origin; ray.TMin = tMin; ray.Direction = dir; ray.TMax = tMax;
    RayQuery<RAY_FLAG_NONE> sq;
    sq.TraceRayInline(topLevelAS, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH, GT_VIS_SOLID, ray);
    RtConfirmAlphaCandidates(sq, pc.geomTable);
    return (sq.CommittedStatus() != COMMITTED_NOTHING) ? 0.0 : 1.0;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    int2 pixel = int2(tid.xy);
    int2 size  = int2(pc.dispatchW, pc.dispatchH);
    if (pixel.x >= size.x || pixel.y >= size.y) return;
    uint flatIdx = uint(pixel.y) * uint(size.x) + uint(pixel.x);

    GIReservoir r; GIReservoirReset(r);

    float depth      = u_SceneDepth.Load(int3(GiGuide(pixel), 0)).r;
    uint  lightCount = LoadPointCount();
    if (depth >= 1.0) { reservoirs[flatIdx] = r; return; }   // sky -> empty sample

    float2 octNv    = u_SlimNormal.Load(int3(GiGuide(pixel), 0)).rg;
    float3 Nv       = OctDecode(octNv);
    float2 uv       = (float2(pixel) + 0.5) / float2(size);
    float4 worldH   = mul(pc.invViewProj, float4(uv * 2.0 - 1.0, depth, 1.0));
    float3 worldPos = worldH.xyz / worldH.w;

    uint seed = PcgHash(flatIdx ^ (pc.frameSeed * 2654435761u));

    // Cosine-sample one bounce; pdf = cos/pi. Sequence the two stream draws into locals (argument
    // evaluation order is unspecified, so an inline float2(rand, rand) would pick u.x/u.y arbitrarily).
    float u0 = RandFromSeed(seed);
    float u1 = RandFromSeed(seed);
    float  pdf;
    float3 d = SampleCosHemisphere(Nv, float2(u0, u1), pdf);

    // Trace the bounce to the closest hit (no terminate flag -> committed = nearest).
    float  tMin   = ubo.rtShadowParams.y;
    float3 origin = worldPos + Nv * ubo.rtShadowParams.z + d * tMin;
    RayDesc ray; ray.Origin = origin; ray.TMin = tMin; ray.Direction = d; ray.TMax = 1.0e30;
    RayQuery<RAY_FLAG_NONE> rq;
    rq.TraceRayInline(topLevelAS, RAY_FLAG_NONE, GT_VIS_ALL, ray);
    RtConfirmAlphaCandidates(rq, pc.geomTable);
    if (rq.CommittedStatus() == COMMITTED_NOTHING) { reservoirs[flatIdx] = r; return; }   // miss -> empty
    float  t  = rq.CommittedRayT();
    float3 xs = origin + d * t;

    // Secondary surface: geometric normal n_s, diffuse albedo, emission.
    float3 ns, baseColor;
    float3 emission = float3(0.0);
    if (GI_USE_SCAFFOLD_LO)
    {
        ns        = normalize(worldPos - xs);
        baseColor = float3(pc.secondaryAlbedo);
    }
    else
    {
        HitSurface hs = FetchHitSurface(pc.geomTable, rq.CommittedInstanceID(), rq.CommittedPrimitiveIndex(),
                                        rq.CommittedTriangleBarycentrics(), rq.CommittedObjectToWorld3x4(), d, xs);
        ns        = hs.ng;   // GI keeps the geometric secondary normal
        baseColor = hs.baseColor;
        emission  = hs.emission;
    }

    // L_o at x_s toward x_v = emission + single-light NEE (Lambertian secondary). One point light picked
    // uniformly; x lightCount divides out the 1/lightCount source pdf.
    float3 Lo = emission;
    if (lightCount > 0u)
    {
        uint j = min(uint(RandFromSeed(seed) * float(lightCount)), lightCount - 1u);
        PointLightData pl = LoadPoint(j);
        float3 Lw; float ld;
        float3 Li  = PointLightRadiance(pl, xs, Lw, ld);
        float  ndl = max(dot(ns, Lw), 0.0);
        if (ndl > 0.0)
        {
            float  sMin  = ubo.rtShadowParams.y;
            float3 sOrig = xs + ns * (ubo.rtShadowParams.z * 0.5) + Lw * sMin;
            float  vis   = TraceVisibility(sOrig, Lw, sMin, max(ld - 2.0 * sMin, sMin));
            Lo += (baseColor / GI_PI) * Li * ndl * vis * float(lightCount);
        }
    }

    // Sun (directional) NEE: deterministic every path (lower-noise for the dominant light). Diffuse only,
    // matching the raster dirRadiance convention (color*intensity, no 1/d^2). One shadow ray to infinity.
    DirectionalLightData dl = LoadDirLight();
    if (dl.intensity > 0.0)
    {
        float3 Lsun   = normalize(-dl.direction);
        float  ndlSun = max(dot(ns, Lsun), 0.0);
        if (ndlSun > 0.0)
        {
            float  sMin  = ubo.rtShadowParams.y;
            float3 sOrig = xs + ns * (ubo.rtShadowParams.z * 0.5) + Lsun * sMin;
            float  vis   = TraceVisibility(sOrig, Lsun, sMin, 1.0e30);
            Lo += (baseColor / GI_PI) * dl.color * dl.intensity * ndlSun * vis;
        }
    }

    // Firefly clamp on the secondary radiance before it enters spatiotemporal reuse.
    float lum = Luminance(Lo);
    if (lum > pc.maxIndirect) Lo *= pc.maxIndirect / lum;

    // Single-candidate reservoir; GIReservoirUpdate seeds the sample even at w=0 (a valid x_s; see
    // restir_gi_common). W -> 1/pdf when lit (target cancels in finalize), 0 when unlit.
    float pHat = GiTargetFunction(xs, Lo, worldPos, Nv);
    float w    = (pdf > 0.0) ? pHat / pdf : 0.0;
    GIReservoirUpdate(r, xs, Lo, OctEncode(ns), w, 0.5);
    GIReservoirFinalize(r, pHat);
    r.age             = 0u;
    r.visPos          = worldPos;                                       // self-carried receiver pos
    r.visNormalPacked = f32tof16(octNv.x) | (f32tof16(octNv.y) << 16);  // self-carried receiver normal

    reservoirs[flatIdx] = r;
}
