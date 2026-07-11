// RT specular reflections. One GGX-VNDF reflection ray per opaque pixel below a
// roughness cutoff, traced from the slim G-buffer, shaded at the hit (NEE + emission + diffuse IBL) or the
// sharp environment on miss. Outputs lobe-mean incoming radiance (rgb) + world hitDist (a) for the
// specular denoiser; pbr.frag composites it. Opaque-only. see arch/rendering-pipeline.md
import globals;
import restir_common;       // light structs + PCG/luminance/oct + PointLightRadiance
import restir_gi_common;    // BranchlessONB + GI_PI
import brdf;                // SampleGGXVNDF (Heitz 2018)
import material;
import material_bindings_rt; // FetchHitSurface + RtConfirmAlphaCandidates (Set 3 material, Set 4 bindless)

// Set 0 b1/b2: IBL irradiance + prefiltered env; b6: TLAS (b0 = GlobalUniforms via globals).
[[vk::binding(1, 0)]] SamplerCube u_IrradianceMap;
[[vk::binding(2, 0)]] SamplerCube u_PrefilteredEnv;
[[vk::binding(6, 0)]] RaytracingAccelerationStructure topLevelAS;
// Set 1 b0: LightSSBO (byte-loaded). Set 2: refl output (rgb=radiance,a=hitDist) + depth/normal/roughness.
[[vk::binding(0, 1)]] ByteAddressBuffer g_Lights;
[[vk::binding(0, 2)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> u_Refl;
[[vk::binding(1, 2)]] Sampler2D u_SceneDepth;
[[vk::binding(2, 2)]] Sampler2D u_SlimNormal;      // RG16F oct
[[vk::binding(3, 2)]] Sampler2D u_SlimRoughness;   // R8

struct PC
{
    float4x4     invViewProj;
    uint         frameSeed;
    float        roughnessCutoff;   // skip the trace above this (rough -> prefiltered-env IBL fallback)
    float        maxRayDistance;
    float        fireflyClamp;
    uint         envReady;          // 1 -> IBL prefiltered env bound
    int          gbufferScale;      // 1 = full-res; 2 = half-res (G-buffer reads remap to full)
    int          dispatchW;         // reflection working (dispatch) resolution
    int          dispatchH;
    GtGeomEntry* geomTable;
    float        minLobeAlpha;   // GGX rough^2 floor: near-mirror 1-spp point-samples bright hits -> fireflies
    float        neeClamp;       // luminance cap on the point-light NEE term (the x-count un-pdf spike)
};
[[vk::push_constant]] PC pc;

// Map a working-res pixel to the full-res G-buffer texel (depth/normal/roughness live at full res).
int2 GbufCoord(int2 p)
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

// Outgoing radiance toward the receiver at the reflection hit: emission + point+sun NEE (Lambertian) + a
// cheap diffuse-IBL ambient. Shading normal for lighting, geometric for ray offsets. 1 bounce.
float3 ShadeHit(float3 xs, HitSurface hs, inout uint seed)
{
    float3 ns      = hs.ns;
    float3 diffAlb = hs.baseColor * (1.0 - hs.metalness);   // metals carry no Lambertian diffuse
    float3 Lo      = hs.emission;

    if ((LightPointCount(g_Lights) + LightTriCount(g_Lights)) > 0u)
    {
        uint lightIndex; uint uvPacked; float invSourcePdf;
        DrawLightCandidate(g_Lights, seed, lightIndex, uvPacked, invSourcePdf);
        LightSample s = ReSampleLight(g_Lights, lightIndex, uvPacked, xs);
        float ndl = max(dot(ns, s.L), 0.0);
        if (ndl > 0.0 && Visible(xs, hs.ng, s.L, s.dist - 2.0 * ubo.rtShadowParams.y))
        {
            // invSourcePdf undoes the alias/uniform pick pdf; that single-light lottery is the NEE spike
            // source, so clamp this term (not sun/emission/IBL) before it enters the 1-spp reflection.
            float3 nee  = (diffAlb / GI_PI) * s.Li * ndl * invSourcePdf;
            float  nlum = Luminance(nee);
            if (nlum > pc.neeClamp) nee *= pc.neeClamp / nlum;
            Lo += nee;
        }
    }
    DirectionalLightData dl = LoadDirLight();
    if (dl.intensity > 0.0)
    {
        float3 Lsun   = normalize(-dl.direction);
        float  ndlSun = max(dot(ns, Lsun), 0.0);
        if (ndlSun > 0.0 && Visible(xs, hs.ng, Lsun, 1.0e30))
            Lo += (diffAlb / GI_PI) * dl.color * dl.intensity * ndlSun;
    }
    Lo += u_IrradianceMap.SampleLevel(ns, 0.0).rgb * diffAlb * ubo.iblIntensity * hs.ao;
    return Lo;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    int2 pixel = int2(tid.xy);
    int2 size  = int2(pc.dispatchW, pc.dispatchH);
    if (pixel.x >= size.x || pixel.y >= size.y) return;
    int2 gp    = GbufCoord(pixel);

    // Every pixel is written (persistent image): sky / rough / below-horizon write zero, and pbr.frag's
    // reflWeight falls back to prefiltered-env IBL there.
    float depth = u_SceneDepth.Load(int3(gp, 0)).r;
    if (depth >= 1.0)               { u_Refl[uint2(pixel)] = float4(0.0); return; }
    float rough = u_SlimRoughness.Load(int3(gp, 0)).r;
    if (rough > pc.roughnessCutoff) { u_Refl[uint2(pixel)] = float4(0.0); return; }

    float3 N        = OctDecode(u_SlimNormal.Load(int3(gp, 0)).rg);
    float2 uv       = (float2(pixel) + 0.5) / float2(size);
    float4 worldH   = mul(pc.invViewProj, float4(uv * 2.0 - 1.0, depth, 1.0));
    float3 worldPos = worldH.xyz / worldH.w;
    float3 V        = normalize(ubo.cameraPos - worldPos);

    uint seed = PcgHash((uint(pixel.y) * uint(size.x) + uint(pixel.x)) ^ (pc.frameSeed * 2654435761u));

    // GGX-VNDF reflection ray: sample a visible half-vector in tangent space (N = +Z), reflect V.
    float3 t, bt; BranchlessONB(N, t, bt);
    float3 Vt = float3(dot(V, t), dot(V, bt), dot(V, N));
    float3 Ht = SampleGGXVNDF(Vt, max(rough * rough, pc.minLobeAlpha), RandFromSeed(seed), RandFromSeed(seed));
    float3 H  = Ht.x * t + Ht.y * bt + Ht.z * N;
    float3 R  = reflect(-V, H);
    if (dot(N, R) <= 0.0)           { u_Refl[uint2(pixel)] = float4(0.0); return; }   // below horizon

    // Trace to the nearest hit; origin offset grows as R grazes (slim normal != geometric -> self-hit guard).
    float  tMin = ubo.rtShadowParams.y;
    float3 ro   = worldPos + N * (ubo.rtShadowParams.z * (2.0 - dot(N, R))) + R * tMin;
    RayDesc ray; ray.Origin = ro; ray.TMin = tMin; ray.Direction = R; ray.TMax = pc.maxRayDistance;
    RayQuery<RAY_FLAG_NONE> rq;
    rq.TraceRayInline(topLevelAS, RAY_FLAG_NONE, GT_VIS_ALL, ray);
    RtConfirmAlphaCandidates(rq, pc.geomTable);

    float3 Li;
    float  hitDist;
    if (rq.CommittedStatus() == COMMITTED_NOTHING)
    {
        // Miss -> SHARP environment along R (LOD 0; the VNDF already spread R), x iblIntensity.
        Li      = (pc.envReady != 0u) ? u_PrefilteredEnv.SampleLevel(R, 0.0).rgb * ubo.iblIntensity : float3(0.0);
        hitDist = pc.maxRayDistance;
    }
    else
    {
        float  tHit = rq.CommittedRayT();
        float3 xs   = ro + R * tHit;
        HitSurface hs = FetchHitSurface(pc.geomTable, rq.CommittedInstanceID(), rq.CommittedPrimitiveIndex(),
                                        rq.CommittedTriangleBarycentrics(), rq.CommittedObjectToWorld3x4(), R, xs);
        Li      = ShadeHit(xs, hs, seed);
        hitDist = tHit;
    }

    // Firefly clamp the 1-spp radiance before the temporal denoiser.
    float lum = Luminance(Li);
    if (lum > pc.fireflyClamp) Li *= pc.fireflyClamp / lum;

    u_Refl[uint2(pixel)] = float4(Li, hitDist);
}
