// SVGF variance-guided edge-aware a-trous wavelet (Schied 2017). One level of the wavelet transform —
// dispatched N times with a doubling step size for a 65x65 effective footprint. The 5x5 cross-bilateral
// filter is gated by depth (gradient-normalized), world-normal (power), and luminance (driven by the moment
// variance) edge-stops. Variance is co-filtered with the SQUARED weights; the luminance weight uses a 3x3
// gaussian prefilter of the variance (not propagated). Pure post-filter: temporal feedback is the
// reproject's integrated color (feedbackTap = -1); the final level writes the consumed image.
// see arch/rendering-pipeline.md
import globals;
import svgf_common;

[[vk::binding(0, 1)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> u_AtrousIn;    // color (rgb) + variance (a)
[[vk::binding(1, 1)]] Sampler2D u_Depth;
[[vk::binding(2, 1)]] Sampler2D u_Normal;
[[vk::binding(3, 1)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> outAtrous;      // next a-trous level
[[vk::binding(4, 1)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> outDenoised;    // consumed (final level)

struct PC
{
    int   stepSize;     // a-trous hole size (1, 2, 4, ...)
    int   writeFinal;   // 1 on the last level — also write the denoised output
    float phiColor;     // luminance edge-stop sigma
    float phiNormal;    // normal edge-stop exponent
    float phiDepth;     // depth edge-stop scale
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

static const float kKernel[3] = { 1.0, 2.0 / 3.0, 1.0 / 6.0 };  // unnormalized 5-tap B-spline half

float linZAt(int2 p) { return SvgfLinearizeDepth(u_Depth.Load(int3(SvgfGuide(p), 0)).r); }

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    int2 pixel = int2(tid.xy);
    int2 size  = int2(pc.dispatchW, pc.dispatchH);
    if (pixel.x >= size.x || pixel.y >= size.y) return;

    float4 c     = u_AtrousIn[pixel];
    float  lc    = SvgfLuminance(c.rgb);
    float  depth = u_Depth.Load(int3(SvgfGuide(pixel), 0)).r;

    if (depth >= 1.0)            // sky — passthrough
    {
        outAtrous[pixel] = c;
        if (pc.writeFinal != 0) outDenoised[pixel] = float4(c.rgb, 1.0);
        return;
    }

    float  lzc = SvgfLinearizeDepth(depth);
    float3 Nc  = SvgfOctDecode(u_Normal.Load(int3(SvgfGuide(pixel), 0)).rg);

    // Central-difference linear-depth gradient (compute substitute for fwidth).
    float dzdx = 0.5 * (linZAt(min(pixel + int2(1, 0), size - int2(1))) - linZAt(max(pixel - int2(1, 0), int2(0))));
    float dzdy = 0.5 * (linZAt(min(pixel + int2(0, 1), size - int2(1))) - linZAt(max(pixel - int2(0, 1), int2(0))));

    // 3x3 gaussian of the variance — drives the luminance weight only (not propagated).
    float gaussVar = 0.0, gaussW = 0.0;
    const float g2[3] = { 1.0, 2.0, 1.0 };
    for (int yy = -1; yy <= 1; ++yy)
    for (int xx = -1; xx <= 1; ++xx)
    {
        int2  q  = clamp(pixel + int2(xx, yy), int2(0), size - int2(1));
        float gw = g2[xx + 1] * g2[yy + 1];
        gaussVar += gw * u_AtrousIn[q].a;
        gaussW   += gw;
    }
    float phiL = pc.phiColor * sqrt(max(0.0, gaussVar / gaussW)) + 1e-8;

    float3 sumColor = c.rgb;
    float  sumVar   = c.a;
    float  sumW     = 1.0;       // center weight = 1
    for (int yy = -2; yy <= 2; ++yy)
    for (int xx = -2; xx <= 2; ++xx)
    {
        if (xx == 0 && yy == 0) continue;
        int2 q = pixel + int2(xx, yy) * pc.stepSize;
        if (any(q < int2(0)) || any(q >= size)) continue;
        float dq = u_Depth.Load(int3(SvgfGuide(q), 0)).r;
        if (dq >= 1.0) continue;

        float4 cq  = u_AtrousIn[q];
        float  lq  = SvgfLuminance(cq.rgb);
        float  lzq = SvgfLinearizeDepth(dq);
        float3 Nq  = SvgfOctDecode(u_Normal.Load(int3(SvgfGuide(q), 0)).rg);

        float2 off = float2(xx, yy) * float(pc.stepSize);
        float  expectedDz = abs(dzdx * off.x + dzdy * off.y);
        float  wz = exp(-abs(lzc - lzq) / (pc.phiDepth * expectedDz + 1e-4));
        float  wn = pow(max(0.0, dot(Nc, Nq)), pc.phiNormal);
        float  wl = exp(-abs(lc - lq) / phiL);

        float kernel = kKernel[abs(xx)] * kKernel[abs(yy)];
        float w = kernel * wz * wn * wl;
        sumColor += w * cq.rgb;
        sumVar   += w * w * cq.a;   // variance co-filtered with squared weights
        sumW     += w;
    }

    float3 outColor = sumColor / sumW;
    float  outVar   = sumVar / (sumW * sumW);
    outVar = (isinf(outVar) || isnan(outVar)) ? 0.0 : outVar;
    outAtrous[pixel] = float4(outColor, outVar);
    if (pc.writeFinal != 0) outDenoised[pixel] = float4(outColor, 1.0);
}
