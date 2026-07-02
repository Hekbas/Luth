// SVGF temporal accumulation (Schied 2017). Reprojects the previous frame's integrated color + luminance
// moments through the motion vector (2x2 bilinear, each tap depth/normal validated), blends the current
// noisy DI with an EMA, and estimates per-pixel luminance variance from the accumulated moments. Per-pixel
// geometry (linear depth + oct normal) is carried in a history buffer for next frame's disocclusion (no
// fwidth in compute -> relative linear-depth tolerance + normal dot). see arch/rendering-pipeline.md
//
// invariant: prev/curr history buffers selected by frame parity in C++; set[p] binds prev = hist[p^1]
// (samplers b4-b6) and curr = hist[p] (storage b7-b9).
import globals;
import svgf_common;

[[vk::binding(0, 1)]] Sampler2D u_DiIn;          // current noisy demodulated DI
[[vk::binding(1, 1)]] Sampler2D u_CurrDepth;
[[vk::binding(2, 1)]] Sampler2D u_CurrNormal;    // slim oct normal
[[vk::binding(3, 1)]] Sampler2D u_Motion;        // slim NDC motion delta
[[vk::binding(4, 1)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> u_ColorHistPrev;  // prev color + variance
[[vk::binding(5, 1)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> u_MomentsPrev;    // prev (mu1, mu2, histLen)
[[vk::binding(6, 1)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> u_GeomPrev;       // prev (linearZ, octN.x, octN.y)
[[vk::binding(7, 1)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> outColor;         // curr color + variance
[[vk::binding(8, 1)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> outMoments;       // curr (mu1, mu2, histLen)
[[vk::binding(9, 1)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> outGeom;          // curr (linearZ, octN.x, octN.y)

struct PC
{
    float alphaColor;       // steady-state EMA alpha (color)
    float alphaMoments;     // steady-state EMA alpha (moments)
    float historyCap;       // max history length; per-pixel alpha floor = 1/historyCap
    float depthThreshold;   // relative linear-depth disocclusion tolerance
    float normalThreshold;  // min dot(prevN, currN) to accept a tap
    int   gbufferScale;     // 1 = full-res; >1 = working res below the full G-buffer (half-res GI)
    int   dispatchW;
    int   dispatchH;
};
[[vk::push_constant]] PC pc;

int2 SvgfGuide(int2 p)
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

    float2 octN    = u_CurrNormal.Load(int3(SvgfGuide(pixel), 0)).rg;
    float3 N       = SvgfOctDecode(octN);
    float  depth   = u_CurrDepth.Load(int3(SvgfGuide(pixel), 0)).r;
    float  curLinZ = SvgfLinearizeDepth(depth);

    // Carry this pixel's geometry forward for next frame's disocclusion test (all pixels).
    outGeom[pixel] = float4(curLinZ, octN.x, octN.y, 0.0);

    // Sky / background: no DI to accumulate; reset history so a later disocclusion starts clean.
    if (depth >= 1.0)
    {
        outColor[pixel]   = float4(0.0);
        outMoments[pixel] = float4(0.0);
        return;
    }

    // Clamp the HDR signal to f16 range; clamp the luminance so mu2 = lum^2 stays inside f16 (65504).
    float3 di     = clamp(u_DiIn.Load(int3(pixel, 0)).rgb, 0.0, 65504.0);
    float  curLum = min(SvgfLuminance(di), 255.0);

    // Reproject (TAA convention: motion is an NDC delta; UV offset = motion * 0.5).
    float2 uv       = (float2(pixel) + 0.5) / float2(size);
    float2 prevUV   = uv - u_Motion.Load(int3(SvgfGuide(pixel), 0)).rg * 0.5;
    float2 prevPixF = prevUV * float2(size) - 0.5;   // center the 2x2 footprint on the sample
    int2   p0       = int2(floor(prevPixF));
    float2 fr       = prevPixF - float2(p0);
    float  bw[4]    = { (1.0 - fr.x) * (1.0 - fr.y), fr.x * (1.0 - fr.y),
                        (1.0 - fr.x) * fr.y,         fr.x * fr.y };
    int2   off[4]   = { int2(0, 0), int2(1, 0), int2(0, 1), int2(1, 1) };

    float3 pColor = float3(0.0);
    float2 pMom   = float2(0.0);   // mu1, mu2
    float  pHist  = 0.0;
    float  sumw   = 0.0;
    bool inBounds = all(prevUV >= float2(0.0)) && all(prevUV <= float2(1.0));
    if (inBounds)
    {
        for (int i = 0; i < 4; ++i)
        {
            int2 tp = p0 + off[i];
            if (any(tp < int2(0)) || any(tp >= size)) continue;

            float3 g        = u_GeomPrev[tp].rgb;      // prev (linearZ, octN.x, octN.y)
            float  prevLinZ = g.r;
            float3 prevN    = SvgfOctDecode(g.gb);
            bool ok = abs(curLinZ - prevLinZ) < pc.depthThreshold * curLinZ
                   && dot(prevN, N) > pc.normalThreshold;
            if (!ok) continue;

            float w = bw[i];
            pColor += w * u_ColorHistPrev[tp].rgb;
            float3 m = u_MomentsPrev[tp].rgb;          // mu1, mu2, histLen
            pMom   += w * m.rg;
            pHist  += w * m.b;
            sumw   += w;
        }
    }

    float  histLen;
    float3 color;
    float2 mom;
    if (sumw > 0.01)
    {
        float inv = 1.0 / sumw;
        pColor *= inv; pMom *= inv; pHist *= inv;
        histLen  = min(pc.historyCap, pHist + 1.0);
        float a  = max(pc.alphaColor,   1.0 / histLen);
        float am = max(pc.alphaMoments, 1.0 / histLen);
        color = lerp(pColor, di, a);
        mom   = lerp(pMom, float2(curLum, curLum * curLum), am);
    }
    else
    {
        // Disocclusion: discard history, full replace.
        histLen = 1.0;
        color   = di;
        mom     = float2(curLum, curLum * curLum);
    }

    float variance = mom.y - mom.x * mom.x;
    variance = (isinf(variance) || isnan(variance)) ? 0.0 : max(0.0, variance);
    outColor[pixel]   = float4(color, variance);
    outMoments[pixel] = float4(mom, histLen, 0.0);
}
