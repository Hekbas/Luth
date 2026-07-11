// ReSTIR DI initial candidate generation (Bitterli 2020). Per pixel: RIS over M uniformly-sampled point
// lights (unshadowed combined diffuse+spec target), then ONE rayQuery shadow ray to the selected
// light (visibility-in-target). Writes a per-pixel reservoir; restir_shade consumes it. AsyncCompute.
// see arch/rendering-pipeline.md
import globals;
import restir_common;       // Reservoir + PCG/oct + PointLightRadiance
import restir_di_target;    // RestirTargetPdf
import material;
import material_bindings_rt; // RtConfirmAlphaCandidates (Set 3 material, Set 4 bindless)

[[vk::binding(6, 0)]] RaytracingAccelerationStructure topLevelAS;
[[vk::binding(0, 1)]] ByteAddressBuffer g_Lights;
[[vk::binding(0, 2)]] Sampler2D u_SceneDepth;
[[vk::binding(1, 2)]] Sampler2D u_SlimNormal;     // RG16F oct
[[vk::binding(2, 2)]] RWStructuredBuffer<Reservoir> reservoirs;
[[vk::binding(7, 2)]] Sampler2D u_SlimRoughness;  // combined diffuse+spec target

struct PC
{
    float4x4     invViewProj;
    uint         candidateCount;   // M
    uint         frameSeed;
    int          gbufferScale;      // 1 = full-res; 2 = half-res DI (G-buffer reads remap to full)
    int          dispatchW;         // DI working (dispatch) resolution
    int          dispatchH;
    GtGeomEntry* geomTable;
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

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    int2 pixel = int2(tid.xy);
    int2 size  = int2(pc.dispatchW, pc.dispatchH);
    if (pixel.x >= size.x || pixel.y >= size.y) return;
    uint flatIdx = uint(pixel.y) * uint(size.x) + uint(pixel.x);

    Reservoir r; ReservoirReset(r);

    float depth      = u_SceneDepth.Load(int3(GbufCoord(pixel), 0)).r;
    uint  lightCount = LoadPointCount();
    if (depth >= 1.0 || lightCount == 0u) { reservoirs[flatIdx] = r; return; }   // sky / no lights

    float2 uv       = (float2(pixel) + 0.5) / float2(size);
    float2 uvR      = DejitterReconUV(uv, ubo.taaParams.zw);   // un-jittered recon; see globals
    float4 worldH   = mul(pc.invViewProj, float4(uvR * 2.0 - 1.0, depth, 1.0));
    float3 worldPos = worldH.xyz / worldH.w;
    float3 N        = OctDecode(u_SlimNormal.Load(int3(GbufCoord(pixel), 0)).rg);
    float3 V        = normalize(ubo.cameraPos - worldPos);
    float  rough    = u_SlimRoughness.Load(int3(GbufCoord(pixel), 0)).r;

    uint  seed = PcgHash(flatIdx ^ pc.frameSeed);
    uint  M    = max(pc.candidateCount, 1u);
    float sourceScale = float(lightCount);   // uniform source pdf = 1/lightCount -> w = pHat x lightCount

    for (uint i = 0u; i < M; ++i)
    {
        uint idx = min(uint(RandFromSeed(seed) * float(lightCount)), lightCount - 1u);
        PointLightData pl = LoadPoint(idx);
        float3 L; float dist;
        float3 Li   = PointLightRadiance(pl, worldPos, L, dist);
        float  pHat = RestirTargetPdf(N, V, L, Li, rough);   // unshadowed diffuse+spec (albedo-free)
        ReservoirUpdate(r, idx, pHat, pHat * sourceScale, RandFromSeed(seed));
    }
    ReservoirFinalize(r);

    // Visibility of the selected sample: fold into the reservoir (occluded -> no contribution).
    if (r.lightIndex != RESTIR_NO_LIGHT && r.W > 0.0)
    {
        PointLightData pl = LoadPoint(r.lightIndex);
        float3 toLight = pl.position - worldPos;
        float  dist    = length(toLight);
        float3 L       = toLight / max(dist, 1e-4);
        float  tMin    = ubo.rtShadowParams.y;
        float3 origin  = worldPos + N * (ubo.rtShadowParams.z * 0.5) + L * tMin;
        RayDesc ray; ray.Origin = origin; ray.TMin = tMin; ray.Direction = L; ray.TMax = max(dist - 2.0 * tMin, tMin);
        RayQuery<RAY_FLAG_NONE> rq;
        rq.TraceRayInline(topLevelAS, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH, GT_VIS_SOLID, ray);
        RtConfirmAlphaCandidates(rq, pc.geomTable);
        if (rq.CommittedStatus() != COMMITTED_NOTHING) r.W = 0.0;
    }

    reservoirs[flatIdx] = r;
}
