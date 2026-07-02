// ReSTIR GI temporal reuse (Ouyang 2021). Reprojects each pixel to the previous frame via the motion
// vector and merges the previous reservoir into this frame's fresh candidate, confidence-weighted (Bitterli
// relative M-cap) and age-capped. Because a GI reservoir stores a WORLD-SPACE sample point, reusing the
// previous sample at the current receiver is a reconnection SHIFT: the resampling weight is multiplied by
// the reconnection Jacobian (Eq. 11). Runs between initial and shade; the merged result stays in CURR (b2).
// AsyncCompute. see arch/rendering-pipeline.md
import globals;
import restir_common;        // OctDecode / PcgHash / RandFromSeed
import restir_gi_common;     // GIReservoir + merge + GiTargetFunction + Jacobian

[[vk::binding(0, 2)]] Sampler2D u_CurrDepth;
[[vk::binding(1, 2)]] Sampler2D u_CurrNormal;
[[vk::binding(2, 2)]] RWStructuredBuffer<GIReservoir> curr;
[[vk::binding(4, 2)]] StructuredBuffer<GIReservoir> prevRes;
[[vk::binding(5, 2)]] Sampler2D u_Motion;

struct PC
{
    float4x4 invViewProj;
    uint     mCapMaxAge;       // mCap in low 16 bits, maxReservoirAge in high 16 bits
    uint     frameSeed;
    float    depthThreshold;   // disocclusion tolerance: fraction of view distance
    float    normalThreshold;  // min dot(prevN, currN)
    int      gbufferScale;     // 1 = full-res; 2 = half-res GI (G-buffer reads remap to full)
    int      dispatchW;
    int      dispatchH;
};
[[vk::push_constant]] PC pc;

int2 GiGuide(int2 p)
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

    float depth = u_CurrDepth.Load(int3(GiGuide(pixel), 0)).r;
    if (depth >= 1.0) return;   // sky; leave the (empty) candidate untouched

    GIReservoir r   = curr[flatIdx];   // this frame's fresh candidate (from initial)
    float2 octN     = u_CurrNormal.Load(int3(GiGuide(pixel), 0)).rg;
    float3 N        = OctDecode(octN);
    float2 uv       = (float2(pixel) + 0.5) / float2(size);
    float4 worldH   = mul(pc.invViewProj, float4(uv * 2.0 - 1.0, depth, 1.0));
    float3 worldPos = worldH.xyz / worldH.w;

    const uint mCap   = pc.mCapMaxAge & 0xFFFFu;
    const uint maxAge = pc.mCapMaxAge >> 16;

    uint seed = PcgHash(flatIdx ^ (pc.frameSeed * 2246822519u));

    // Start with this frame's candidate: already at this pixel, identity shift. GIReservoirMerge seeds
    // the sample on the first merge even at w=0 (confident-zero invariant: M>0 => samplePos valid).
    GIReservoir c; GIReservoirReset(c);
    if (r.M > 0u)
    {
        float pHatCurr = GiTargetFunction(r.samplePos, r.sampleRadiance, worldPos, N);
        GIReservoirMerge(c, r.samplePos, r.sampleRadiance, r.sampleNormalOct, r.age,
                         pHatCurr, r.W, r.M, RandFromSeed(seed));
    }

    // Reproject (TAA convention: motion is NDC delta; UV offset = motion * 0.5).
    float2 prevUV = uv - u_Motion.Load(int3(GiGuide(pixel), 0)).rg * 0.5;
    if (all(prevUV >= float2(0.0)) && all(prevUV <= float2(1.0)))
    {
        int2 pPix = clamp(int2(prevUV * float2(size)), int2(0), size - int2(1));
        GIReservoir pr = prevRes[uint(pPix.y) * uint(size.x) + uint(pPix.x)];

        float3 prevN = OctDecode(float2(f16tof32(pr.visNormalPacked & 0xFFFFu), f16tof32(pr.visNormalPacked >> 16u)));
        bool valid = pr.M > 0u
                  && pr.age < maxAge
                  && dot(prevN, N) > pc.normalThreshold
                  && length(pr.visPos - worldPos) < pc.depthThreshold * length(worldPos - ubo.cameraPos);

        if (valid)
        {
            float3 sampleN = OctDecode(pr.sampleNormalOct);
            float  jac     = GiReconnectionJacobian(worldPos, pr.visPos, pr.samplePos, sampleN);
            if (GiValidateJacobian(jac))
            {
                float pHatPrev = GiTargetFunction(pr.samplePos, pr.sampleRadiance, worldPos, N);
                uint  mClamped = min(pr.M, mCap * max(r.M, 1u));
                GIReservoirMerge(c, pr.samplePos, pr.sampleRadiance, pr.sampleNormalOct, pr.age + 1u,
                                 pHatPrev * jac, pr.W, mClamped, RandFromSeed(seed));
            }
        }
    }

    // Finalize with the selected sample's target re-evaluated here, then carry receiver geometry forward.
    float pHatSel = GiTargetFunction(c.samplePos, c.sampleRadiance, worldPos, N);
    GIReservoirFinalize(c, pHatSel);
    c.visPos          = worldPos;
    c.visNormalPacked = f32tof16(octN.x) | (f32tof16(octN.y) << 16u);
    curr[flatIdx] = c;
}
