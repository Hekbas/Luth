// ReSTIR DI — spatial reuse (Bitterli 2020). Each pixel merges its own reservoir with a few random
// neighbours inside a disk, rejecting geometrically dissimilar ones (linear depth + normal). Punctual
// point lights store a light INDEX, so the shift map is identity and there is NO Jacobian — the
// neighbour's light target is simply re-evaluated at this pixel. Reads the temporal output (in) and
// writes a separate output (out) — neighbours must be read un-modified, so this can't be in-place.
// AsyncCompute. see arch/rendering-pipeline.md
import globals;
import restir_common;
import restir_di_target;    // #154 RestirTargetPdf

[[vk::binding(0, 1)]] ByteAddressBuffer g_Lights;
[[vk::binding(0, 2)]] Sampler2D u_Depth;
[[vk::binding(1, 2)]] Sampler2D u_Normal;
[[vk::binding(2, 2)]] StructuredBuffer<Reservoir> resIn;
[[vk::binding(6, 2)]] RWStructuredBuffer<Reservoir> resOut;
[[vk::binding(7, 2)]] Sampler2D u_SlimRoughness;   // #154 combined diffuse+spec target

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

static const float k_NormalThreshold = 0.9;

float LinearizeDepth(float d) { return (ubo.nearZ * ubo.farZ) / (ubo.farZ - d * (ubo.farZ - ubo.nearZ)); }

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    int2 pixel = int2(tid.xy);
    int2 size  = int2(pc.dispatchW, pc.dispatchH);
    if (pixel.x >= size.x || pixel.y >= size.y) return;
    uint flatIdx = uint(pixel.y) * uint(size.x) + uint(pixel.x);

    Reservoir r     = resIn[flatIdx];
    float     depth = u_Depth.Load(int3(GbufCoord(pixel), 0)).r;
    if (depth >= 1.0) { resOut[flatIdx] = r; return; }   // sky — pass through

    float3 N        = OctDecode(u_Normal.Load(int3(GbufCoord(pixel), 0)).rg);
    float2 uv       = (float2(pixel) + 0.5) / float2(size);
    float4 worldH   = mul(pc.invViewProj, float4(uv * 2.0 - 1.0, depth, 1.0));
    float3 worldPos = worldH.xyz / worldH.w;
    float  currLin  = LinearizeDepth(depth);
    float3 V        = normalize(ubo.cameraPos - worldPos);
    float  rough    = u_SlimRoughness.Load(int3(GbufCoord(pixel), 0)).r;

    uint seed = PcgHash(flatIdx ^ (pc.frameSeed * 2246822519u));

    Reservoir c; ReservoirReset(c);
    ReservoirMerge(c, r.lightIndex, r.targetPdf, r.W, r.M, RandFromSeed(seed));   // canonical (self)

    for (uint i = 0u; i < pc.neighbourCount; ++i)
    {
        float ang = RandFromSeed(seed) * 6.28318530718;
        float rad = sqrt(RandFromSeed(seed)) * float(pc.radius);
        int2  np  = pixel + int2(int(round(cos(ang) * rad)), int(round(sin(ang) * rad)));
        if (np.x < 0 || np.y < 0 || np.x >= size.x || np.y >= size.y || all(np == pixel)) continue;

        float nd = u_Depth.Load(int3(GbufCoord(np), 0)).r;
        if (nd >= 1.0) continue;
        float3 nN = OctDecode(u_Normal.Load(int3(GbufCoord(np), 0)).rg);
        if (abs(currLin - LinearizeDepth(nd)) > pc.depthThreshold * currLin) continue;
        if (dot(nN, N) < k_NormalThreshold) continue;

        Reservoir nr = resIn[uint(np.y) * uint(size.x) + uint(np.x)];
        if (nr.lightIndex == RESTIR_NO_LIGHT || nr.M == 0u) continue;

        // Re-evaluate the neighbour's selected light at THIS pixel (punctual → no Jacobian).
        PointLightData pl = LoadPoint(nr.lightIndex);
        float3 L; float dist;
        float3 Li   = PointLightRadiance(pl, worldPos, L, dist);
        float  pHat = RestirTargetPdf(N, V, L, Li, rough);   // (#154 combined target)
        ReservoirMerge(c, nr.lightIndex, pHat, nr.W, nr.M, RandFromSeed(seed));
    }

    ReservoirFinalize(c);
    resOut[flatIdx] = c;
}
