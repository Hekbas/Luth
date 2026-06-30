// ReSTIR DI — temporal reuse (Bitterli 2020). Reprojects each pixel to the previous frame via the motion
// vector and merges the previous final reservoir into this frame's RIS candidate with confidence weights
// (M-capped). The reservoir self-carries its origin pixel's geometry in the _pad fields (raw depth + oct
// normal), so validation needs no separate previous-frame G-buffer. Runs between initial and shade.
// AsyncCompute. see arch/rendering-pipeline.md
import globals;
import restir_common;       // Reservoir + merge/PCG/oct + PointLightRadiance
import restir_di_target;    // RestirTargetPdf (#154)

[[vk::binding(0, 1)]] ByteAddressBuffer g_Lights;
[[vk::binding(0, 2)]] Sampler2D u_CurrDepth;
[[vk::binding(1, 2)]] Sampler2D u_CurrNormal;
[[vk::binding(2, 2)]] RWStructuredBuffer<Reservoir> curr;       // this frame's RIS candidate (read + write)
[[vk::binding(4, 2)]] StructuredBuffer<Reservoir> prevRes;      // previous frame's final reservoirs
[[vk::binding(5, 2)]] Sampler2D u_Motion;
[[vk::binding(7, 2)]] Sampler2D u_SlimRoughness;   // #154 combined diffuse+spec target

struct PC
{
    float4x4 invViewProj;
    uint     mCap;             // history clamp multiplier — prev.M <= mCap * curr.M (Bitterli ~20)
    uint     frameSeed;
    float    depthThreshold;   // relative linear-depth tolerance for history validation
    float    normalThreshold;  // min dot(prevN, currN)
    int      gbufferScale;     // 1 = full-res; 2 = half-res DI (G-buffer reads remap to full)
    int      dispatchW;
    int      dispatchH;
};
[[vk::push_constant]] PC pc;

// LightBuffer: header (DirLight 32 + count 4 + pad 12) then points[] (32 B each) at offset 48.
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

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    int2 pixel = int2(tid.xy);
    int2 size  = int2(pc.dispatchW, pc.dispatchH);
    if (pixel.x >= size.x || pixel.y >= size.y) return;
    uint flatIdx = uint(pixel.y) * uint(size.x) + uint(pixel.x);

    float depth = u_CurrDepth.Load(int3(GbufCoord(pixel), 0)).r;
    if (depth >= 1.0) return;   // sky — leave the (empty) candidate untouched

    Reservoir r     = curr[flatIdx];   // this frame's RIS candidate (visibility folded by restir_initial)
    float2 octN     = u_CurrNormal.Load(int3(GbufCoord(pixel), 0)).rg;
    float3 N        = OctDecode(octN);
    float2 uv       = (float2(pixel) + 0.5) / float2(size);
    float4 worldH   = mul(pc.invViewProj, float4(uv * 2.0 - 1.0, depth, 1.0));
    float3 worldPos = worldH.xyz / worldH.w;
    float3 V        = normalize(ubo.cameraPos - worldPos);
    float  rough    = u_SlimRoughness.Load(int3(GbufCoord(pixel), 0)).r;

    // Reproject (TAA convention: motion is NDC delta; UV offset = motion * 0.5).
    float2 prevUV = uv - u_Motion.Load(int3(GbufCoord(pixel), 0)).rg * 0.5;

    Reservoir pr; ReservoirReset(pr);
    bool valid = all(prevUV >= float2(0.0)) && all(prevUV <= float2(1.0));
    if (valid)
    {
        int2 pPix = clamp(int2(prevUV * float2(size)), int2(0), size - int2(1));
        pr = prevRes[uint(pPix.y) * uint(size.x) + uint(pPix.x)];
        // Validate against the previous reservoir's stored geometry (pad0 = raw depth, pad1/2 = oct).
        float currLin = LinearizeDepth(depth);
        valid = pr.lightIndex != RESTIR_NO_LIGHT && pr.M > 0u
             && abs(currLin - LinearizeDepth(pr._pad0)) < pc.depthThreshold * currLin
             && dot(OctDecode(float2(pr._pad1, pr._pad2)), N) > pc.normalThreshold;
    }

    uint seed = PcgHash(flatIdx ^ (pc.frameSeed * 2654435761u));

    Reservoir c; ReservoirReset(c);
    ReservoirMerge(c, r.lightIndex, r.targetPdf, r.W, r.M, RandFromSeed(seed));   // current (target already at this pixel)

    if (valid)
    {
        PointLightData pl = LoadPoint(pr.lightIndex);
        float3 L; float dist;
        float3 Li   = PointLightRadiance(pl, worldPos, L, dist);
        float  pHat = RestirTargetPdf(N, V, L, Li, rough);   // re-evaluate prev's light at THIS pixel (#154)
        // Relative history clamp (Bitterli 2020): prev confidence capped to mCap x the current frame's M.
        ReservoirMerge(c, pr.lightIndex, pHat, pr.W, min(pr.M, pc.mCap * r.M), RandFromSeed(seed));
    }

    ReservoirFinalize(c);
    // Carry this pixel's geometry forward for next frame's validation.
    c._pad0 = depth;
    c._pad1 = octN.x;
    c._pad2 = octN.y;
    curr[flatIdx] = c;
}
