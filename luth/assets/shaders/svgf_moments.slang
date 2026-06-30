// SVGF variance estimation (Schied 2017). For pixels with enough temporal history (>= 4 frames) variance
// comes straight from the accumulated luminance moments (mu2 - mu1^2, written by reproject). For freshly
// disoccluded pixels (history < 4) the temporal estimate is unreliable, so variance is estimated spatially
// from a 7x7 depth/normal-bilateral filter of the moments, boosted by 4/historyLength. Output is the
// a-trous level-0 input: integrated color (rgb) + variance (a). see arch/rendering-pipeline.md
import globals;
import svgf_common;

[[vk::binding(0, 1)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> u_ColorIn;    // color (rgb) + temporal variance (a)
[[vk::binding(1, 1)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> u_MomentsIn;  // (mu1, mu2, histLen)
[[vk::binding(2, 1)]] Sampler2D u_Depth;
[[vk::binding(3, 1)]] Sampler2D u_Normal;
[[vk::binding(4, 1)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> outAtrous;     // color (rgb) + variance (a)

struct PC
{
    float phiDepth;     // relative-depth bilateral falloff for the spatial fallback
    float phiNormal;    // normal bilateral exponent
    int   gbufferScale;
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

float linZAt(int2 p) { return SvgfLinearizeDepth(u_Depth.Load(int3(SvgfGuide(p), 0)).r); }

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    int2 pixel = int2(tid.xy);
    int2 size  = int2(pc.dispatchW, pc.dispatchH);
    if (pixel.x >= size.x || pixel.y >= size.y) return;

    float4 ci      = u_ColorIn[pixel];
    float3 color   = ci.rgb;
    float3 mom     = u_MomentsIn[pixel].rgb;   // mu1, mu2, histLen
    float  histLen = mom.b;

    float depth = u_Depth.Load(int3(SvgfGuide(pixel), 0)).r;
    if (depth >= 1.0)            // sky — passthrough, no variance
    {
        outAtrous[pixel] = float4(color, 0.0);
        return;
    }

    float variance;
    if (histLen >= 4.0)
    {
        // Enough history — trust the temporal moment variance the reproject already computed.
        variance = max(0.0, ci.a);
    }
    else
    {
        // 7x7 depth/normal-bilateral filter of the moments; variance from the filtered moments.
        float  linZc = SvgfLinearizeDepth(depth);
        float3 Nc    = SvgfOctDecode(u_Normal.Load(int3(SvgfGuide(pixel), 0)).rg);
        // Central-difference linear-depth gradient (compute substitute for fwidth).
        float dzdx = 0.5 * (linZAt(min(pixel + int2(1, 0), size - int2(1))) - linZAt(max(pixel - int2(1, 0), int2(0))));
        float dzdy = 0.5 * (linZAt(min(pixel + int2(0, 1), size - int2(1))) - linZAt(max(pixel - int2(0, 1), int2(0))));
        float2 sumMom = float2(0.0);
        float  sumW   = 0.0;
        for (int yy = -3; yy <= 3; ++yy)
        for (int xx = -3; xx <= 3; ++xx)
        {
            int2 q = pixel + int2(xx, yy);
            if (any(q < int2(0)) || any(q >= size)) continue;
            float dq = u_Depth.Load(int3(SvgfGuide(q), 0)).r;
            if (dq >= 1.0) continue;
            float  linZq = SvgfLinearizeDepth(dq);
            float3 Nq    = SvgfOctDecode(u_Normal.Load(int3(SvgfGuide(q), 0)).rg);
            float  expectedDz = abs(dzdx * float(xx) + dzdy * float(yy));
            float  wz = exp(-abs(linZc - linZq) / (pc.phiDepth * expectedDz + 1e-4));
            float  wn = pow(max(0.0, dot(Nc, Nq)), pc.phiNormal);
            float  w  = wz * wn;
            sumMom += w * u_MomentsIn[q].rg;
            sumW   += w;
        }
        if (sumW > 1e-6)
        {
            float2 m = sumMom / sumW;
            variance = max(0.0, m.y - m.x * m.x) * (4.0 / max(histLen, 1.0));
        }
        else
        {
            variance = max(0.0, mom.y - mom.x * mom.x);
        }
        variance = (isinf(variance) || isnan(variance)) ? 0.0 : variance;
    }

    outAtrous[pixel] = float4(color, variance);
}
