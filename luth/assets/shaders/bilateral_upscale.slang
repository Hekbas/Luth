// Joint-bilateral upscale of a half-res denoised signal (ReSTIR GI / DI / reflections) into its full-res
// image. For each full-res pixel: gather the 4 covering half-res taps, weight by depth + normal edge-stops
// against the full-res guide, with a depth-nearest fallback on total rejection (holds silhouettes against
// light leak). Only runs in half-res mode. see arch/rendering-pipeline.md
import globals;
import svgf_common;

[[vk::binding(0, 1)]] Sampler2D u_Half;     // half-res denoised signal (demodulated E)
[[vk::binding(1, 1)]] Sampler2D u_Depth;    // full-res scene depth
[[vk::binding(2, 1)]] Sampler2D u_Normal;   // full-res slim oct normal
[[vk::binding(3, 1)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> u_Out;   // full-res denoised output

struct PC
{
    int   fullW;
    int   fullH;
    int   halfW;
    int   halfH;
    float phiDepth;     // relative-depth edge-stop falloff
    float phiNormal;    // normal edge-stop exponent
};
[[vk::push_constant]] PC pc;

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    int2 fp = int2(tid.xy);
    if (fp.x >= pc.fullW || fp.y >= pc.fullH) return;

    float dC = u_Depth.Load(int3(fp, 0)).r;
    if (dC >= 1.0) { u_Out[fp] = float4(0.0); return; }   // sky → no GI
    float  zC = SvgfLinearizeDepth(dC);
    float3 nC = SvgfOctDecode(u_Normal.Load(int3(fp, 0)).rg);

    // Half-res sample footprint for this full pixel (bilinear 2x2 base).
    float2 hf = ((float2(fp) + 0.5) / float2(pc.fullW, pc.fullH)) * float2(pc.halfW, pc.halfH) - 0.5;
    int2   h0 = int2(floor(hf));
    float2 fr = hf - float2(h0);
    int2   off[4] = { int2(0, 0), int2(1, 0), int2(0, 1), int2(1, 1) };
    float  bw[4]  = { (1.0 - fr.x) * (1.0 - fr.y), fr.x * (1.0 - fr.y),
                      (1.0 - fr.x) * fr.y,         fr.x * fr.y };

    float3 acc   = float3(0.0);
    float  sumw  = 0.0;
    float3 bestC = float3(0.0);
    float  bestS = -1.0e30;   // depth-nearest fallback score (smallest |dz| wins)
    for (int i = 0; i < 4; ++i)
    {
        int2 ht = clamp(h0 + off[i], int2(0), int2(pc.halfW - 1, pc.halfH - 1));
        // The half tap's full-res guide texel (its center in full space) for the edge-stop weights.
        int2 ft = clamp(int2(((float2(ht) + 0.5) / float2(pc.halfW, pc.halfH)) * float2(pc.fullW, pc.fullH)),
                        int2(0), int2(pc.fullW - 1, pc.fullH - 1));
        float  zT = SvgfLinearizeDepth(u_Depth.Load(int3(ft, 0)).r);
        float3 nT = SvgfOctDecode(u_Normal.Load(int3(ft, 0)).rg);
        float3 cT = u_Half.Load(int3(ht, 0)).rgb;

        float wz = exp(-abs(zC - zT) / (pc.phiDepth * max(zC, 1e-3) + 1e-4));
        float wn = pow(max(0.0, dot(nC, nT)), pc.phiNormal);
        float w  = bw[i] * wz * wn;
        acc  += w * cT;
        sumw += w;

        float s = -abs(zC - zT);
        if (s > bestS) { bestS = s; bestC = cT; }
    }

    float3 outC = (sumw > 1e-5) ? acc / sumw : bestC;   // nearest-depth tap on total edge rejection
    u_Out[fp] = float4(outC, 1.0);
}
