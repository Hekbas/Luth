// ReSTIR DI temporal reuse (Bitterli 2020). Reprojects each pixel to the previous frame via the motion
// vector and merges the previous frame's SPATIAL output (b4: post-spatial history, final visibility
// already folded into W) into this frame's RIS candidate with confidence weights (M-capped). The history
// reservoir self-carries its origin pixel's geometry in the _pad fields (raw depth + oct normal, written
// by the spatial pass), so validation needs no separate previous-frame G-buffer. Reads + writes the
// scratch reservoir in-place. Runs between initial and spatial. AsyncCompute. see arch/rendering-pipeline.md
import globals;
import restir_common;       // Reservoir + merge/PCG/oct + PointLightRadiance
import restir_di_target;    // RestirTargetPdf

[[vk::binding(0, 1)]] ByteAddressBuffer g_Lights;
[[vk::binding(0, 2)]] Sampler2D u_CurrDepth;
[[vk::binding(1, 2)]] Sampler2D u_CurrNormal;
[[vk::binding(2, 2)]] RWStructuredBuffer<Reservoir> curr;       // scratch: this frame's RIS candidate (read + write)
[[vk::binding(4, 2)]] StructuredBuffer<Reservoir> prevRes;      // last frame's SPATIAL output (persistent history)
[[vk::binding(5, 2)]] Sampler2D u_Motion;
[[vk::binding(7, 2)]] Sampler2D u_SlimRoughness;   // combined diffuse+spec target

struct PC
{
    float4x4 invViewProj;
    uint     mCap;             // history clamp multiplier: prev.M <= mCap * curr.M (Bitterli ~20)
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
    if (depth >= 1.0) return;   // sky; leave the (empty) candidate untouched

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
    int2 pPix   = int2(0, 0);
    bool inBnds = all(prevUV >= float2(0.0)) && all(prevUV <= float2(1.0));
    bool geomValid = false;
    if (inBnds)
    {
        pPix = clamp(int2(prevUV * float2(size)), int2(0), size - int2(1));
        pr   = prevRes[uint(pPix.y) * uint(size.x) + uint(pPix.x)];
        // Validate against the previous reservoir's stored geometry (pad0 = raw depth, pad1/2 = oct). A
        // NO_LIGHT prev is still a valid BASIC domain (it owes its M to piSum), so the light-index test is
        // kept separate below: gate domain inclusion on confidence, not on the realized sample (RTXDI).
        float currLin = LinearizeDepth(depth);
        geomValid = pr.M > 0u
                 && abs(currLin - LinearizeDepth(pr._pad0)) < pc.depthThreshold * currLin
                 && dot(OctDecode(float2(pr._pad1, pr._pad2)), N) > pc.normalThreshold;
    }
    bool lightValid = geomValid && pr.lightIndex != RESTIR_NO_LIGHT;

    uint seed = PcgHash(flatIdx ^ (pc.frameSeed * 2654435761u));

    // RTXDI BASIC (generalized balance heuristic) over two domains: this pixel (canonical) and the
    // reprojected previous surface. Streaming weights use each candidate's target at the canonical pixel;
    // the final W re-evaluates the WINNER's light at BOTH surfaces (x that domain's M), so a domain whose
    // target vanishes at the winner is excluded instead of paying full 1/M confidence (the edge darkening).
    // Reconstructing the ACTUAL previous surface is what separates this from biased 1/M: approximate it by
    // the current surface and psPrev == psCurr, collapsing BASIC back to 1/M.
    Reservoir c; ReservoirReset(c);
    int selDomain = -1;
    if (ReservoirMergeSel(c, r.lightIndex, r.targetPdf, r.W, r.M, RandFromSeed(seed))) selDomain = 0;

    float3 prevPos = float3(0.0);
    float3 prevN   = float3(0.0);
    float3 prevV   = float3(0.0);
    uint   Mprev   = 0u;
    if (geomValid)
    {
        // Reconstruct at the prev texel CENTRE (not the jittered prevUV) so the point matches the geometry
        // the reservoir stored at pPix.
        float2 pUv = (float2(pPix) + 0.5) / float2(size);
        prevN   = OctDecode(float2(pr._pad1, pr._pad2));
        float4 pWh = mul(ubo.prevInvViewProjection, float4(pUv * 2.0 - 1.0, pr._pad0, 1.0));
        prevPos = pWh.xyz / pWh.w;
        prevV   = normalize(ubo.prevCameraPos.xyz - prevPos);
        Mprev   = min(pr.M, pc.mCap * max(r.M, 1u));   // Bitterli relative history clamp

        if (lightValid)
        {
            // Streaming weight: prev's light target at THIS (canonical) pixel.
            PointLightData plp = LoadPoint(pr.lightIndex);
            float3 Lp; float dp;
            float3 Lip  = PointLightRadiance(plp, worldPos, Lp, dp);
            float  pHat = RestirTargetPdf(N, V, Lp, Lip, rough);
            if (ReservoirMergeSel(c, pr.lightIndex, pHat, pr.W, Mprev, RandFromSeed(seed))) selDomain = 1;
        }
        else
        {
            c.M += Mprev;   // confident-zero prev: count its M in the denominator, stream no sample
        }
    }

    // BASIC finalize: piSum sums the winner's target at each domain; selTgt is the winner at the canonical
    // pixel. No accepted prev -> selPi == selTgt and piSum == selTgt * M -> W == canonical.W (passthrough).
    float Wout = 0.0;
    if (c.lightIndex != RESTIR_NO_LIGHT && c.M > 0u)
    {
        PointLightData sel = LoadPoint(c.lightIndex);
        float3 Lc; float dc;
        float3 Lic    = PointLightRadiance(sel, worldPos, Lc, dc);
        float  psCurr = RestirTargetPdf(N, V, Lc, Lic, rough);   // selTgt (winner at canonical)
        float  piSum  = psCurr * float(r.M);
        float  selPi  = (selDomain == 0) ? psCurr : 0.0;
        if (geomValid)
        {
            float3 Lp2; float dp2;
            float3 Lip2   = PointLightRadiance(sel, prevPos, Lp2, dp2);
            float  psPrev = RestirTargetPdf(prevN, prevV, Lp2, Lip2, rough);   // winner at prev surface
            piSum += psPrev * float(Mprev);
            if (selDomain == 1) selPi = psPrev;
        }
        Wout = (psCurr > 0.0 && piSum > 0.0) ? (c.wSum * selPi / (psCurr * piSum)) : 0.0;
    }
    c.W = Wout;
    // Geometry self-carry moved to the spatial pass tail: THAT buffer is the history now, not this one.
    curr[flatIdx] = c;
}
