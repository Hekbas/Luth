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
    uint  pointCount = LightPointCount(g_Lights);
    uint  triCount   = LightTriCount(g_Lights);
    if (depth >= 1.0 || (pointCount + triCount) == 0u) { reservoirs[flatIdx] = r; return; }   // sky / no lights

    float2 uv       = (float2(pixel) + 0.5) / float2(size);
    float2 uvR      = DejitterReconUV(uv, ubo.taaParams.zw);   // un-jittered recon; see globals
    float4 worldH   = mul(pc.invViewProj, float4(uvR * 2.0 - 1.0, depth, 1.0));
    float3 worldPos = worldH.xyz / worldH.w;
    float3 N        = OctDecode(u_SlimNormal.Load(int3(GbufCoord(pixel), 0)).rg);
    float3 V        = normalize(ubo.cameraPos - worldPos);
    float  rough    = u_SlimRoughness.Load(int3(GbufCoord(pixel), 0)).r;

    uint seed = PcgHash(flatIdx ^ pc.frameSeed);
    uint M    = max(pc.candidateCount, 1u);

    // RIS candidates. With emissive triangles present, draw from the unified power-weighted alias table over
    // [points | tris] (per-candidate invSourcePdf); else the legacy uniform point draw (zero regression on
    // point-only scenes). Triangle candidates sample a point-on-light (barycentric uv quantized once).
    for (uint i = 0u; i < M; ++i)
    {
        uint  lightIndex; uint uvPacked = 0u; float invSourcePdf;
        if (triCount > 0u)
        {
            uint  numLights = pointCount + triCount;
            uint  bucket    = min(uint(RandFromSeed(seed) * float(numLights)), numLights - 1u);
            float u         = RandFromSeed(seed);
            float pmf; uint sel = AliasSelect(g_Lights, bucket, u, pmf);
            if (sel < pointCount)
            {
                lightIndex   = sel;                                    // point (bit31 = 0)
                invSourcePdf = (pmf > 0.0) ? (1.0 / pmf) : 0.0;
            }
            else
            {
                uint  triLocal = sel - pointCount;
                float su       = sqrt(RandFromSeed(seed));             // uniform-area barycentric warp
                float u2       = RandFromSeed(seed);
                uvPacked       = PackUV(float2(u2 * su, su * (1.0 - u2)));
                lightIndex     = triLocal | LIGHT_TYPE_TRI;
                invSourcePdf   = (pmf > 0.0) ? (LoadTriL(g_Lights, triLocal).area / pmf) : 0.0;   // area measure
            }
        }
        else
        {
            lightIndex   = min(uint(RandFromSeed(seed) * float(pointCount)), pointCount - 1u);
            invSourcePdf = float(pointCount);                          // uniform source pdf = 1/pointCount
        }

        LightSample s = ReSampleLight(g_Lights, lightIndex, uvPacked, worldPos);
        float pHat    = RestirTargetPdf(N, V, s.L, s.Li, rough);       // unshadowed diffuse+spec (albedo-free)
        ReservoirUpdate(r, lightIndex, uvPacked, pHat, pHat * invSourcePdf, RandFromSeed(seed));
    }
    ReservoirFinalize(r);

    // Visibility of the selected sample (point or triangle point-on-light): fold into W (occluded -> 0).
    if (r.lightIndex != RESTIR_NO_LIGHT && r.W > 0.0)
    {
        LightSample s = ReSampleLight(g_Lights, r.lightIndex, r.uvPacked, worldPos);
        float  tMin   = ubo.rtShadowParams.y;
        float3 origin = worldPos + N * (ubo.rtShadowParams.z * 0.5) + s.L * tMin;
        RayDesc ray; ray.Origin = origin; ray.TMin = tMin; ray.Direction = s.L; ray.TMax = max(s.dist - 2.0 * tMin, tMin);
        RayQuery<RAY_FLAG_NONE> rq;
        rq.TraceRayInline(topLevelAS, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH, GT_VIS_SOLID, ray);
        RtConfirmAlphaCandidates(rq, pc.geomTable);
        if (rq.CommittedStatus() != COMMITTED_NOTHING) r.W = 0.0;
    }

    reservoirs[flatIdx] = r;
}
