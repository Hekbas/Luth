// SVGF temporal accumulation, SPECULAR variant. Like svgf_reproject but the
// reprojection is HIT-DISTANCE-DRIVEN: a reflection's screen motion differs from the surface's, so
// reproject by the slim motion vector ghosts mirrors. Instead reconstruct the receiver from depth,
// mirror-reflect the view to the virtual reflected hit (worldPos + reflect(-V,N)*hitDist), and reproject
// THAT through prevViewProjection. The 1-spp radiance + hitDist arrive packed in u_SpecIn (rgb, a); hitDist
// rides the geom history's spare channel; roughness loosens the gate. see arch/rendering-pipeline.md
//
// invariant: prev/curr history selected by frame parity in C++ (set[p] reads [p^1], writes [p]).
import globals;
import svgf_common;

[[vk::binding(0, 1)]] Sampler2D u_SpecIn;      // current 1-spp radiance (rgb) + hitDist (a)
[[vk::binding(1, 1)]] Sampler2D u_CurrDepth;
[[vk::binding(2, 1)]] Sampler2D u_CurrNormal;  // slim oct normal
[[vk::binding(3, 1)]] Sampler2D u_CurrRough;   // slim roughness (R8), disocclusion tuning
[[vk::binding(4, 1)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> u_ColorHistPrev;
[[vk::binding(5, 1)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> u_MomentsPrev;
[[vk::binding(6, 1)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> u_GeomPrev;   // prev (linearZ, octN.x, octN.y, hitDist)
[[vk::binding(7, 1)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> outColor;
[[vk::binding(8, 1)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> outMoments;
[[vk::binding(9, 1)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> outGeom;

struct PC
{
    float alphaColor;
    float alphaMoments;
    float historyCap;
    float depthThreshold;
    float normalThreshold;
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

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    int2 pixel = int2(tid.xy);
    int2 size  = int2(pc.dispatchW, pc.dispatchH);
    if (pixel.x >= size.x || pixel.y >= size.y) return;
    int2 gp = SvgfGuide(pixel);

    float2 octN    = u_CurrNormal.Load(int3(gp, 0)).rg;
    float3 N       = SvgfOctDecode(octN);
    float  depth   = u_CurrDepth.Load(int3(gp, 0)).r;
    float  curLinZ = SvgfLinearizeDepth(depth);
    float4 spec    = u_SpecIn.Load(int3(pixel, 0));
    float  hitDist = max(spec.a, 0.0);
    float  rough   = u_CurrRough.Load(int3(gp, 0)).r;

    // Carry geometry + reflected hit-distance forward for next frame's disocclusion (all pixels).
    outGeom[pixel] = float4(curLinZ, octN.x, octN.y, hitDist);

    if (depth >= 1.0)
    {
        outColor[pixel]   = float4(0.0);
        outMoments[pixel] = float4(0.0);
        return;
    }

    float3 di     = clamp(spec.rgb, 0.0, 65504.0);
    float  curLum = min(SvgfLuminance(di), 255.0);

    // Hit-distance virtual reprojection.
    float2 uv       = (float2(pixel) + 0.5) / float2(size);
    float4 worldH   = mul(ubo.invViewProjection, float4(uv * 2.0 - 1.0, depth, 1.0));
    float3 worldPos = worldH.xyz / worldH.w;
    float3 V        = normalize(ubo.cameraPos - worldPos);
    float3 virtualP = worldPos + reflect(-V, N) * hitDist;
    float4 prevClip = mul(ubo.prevViewProjection, float4(virtualP, 1.0));
    float2 prevUV   = (prevClip.w > 0.0) ? (prevClip.xy / prevClip.w * 0.5 + 0.5) : float2(-1.0);

    float2 prevPixF = prevUV * float2(size) - 0.5;
    int2   p0       = int2(floor(prevPixF));
    float2 fr       = prevPixF - float2(p0);
    float  bw[4]    = { (1.0 - fr.x) * (1.0 - fr.y), fr.x * (1.0 - fr.y),
                        (1.0 - fr.x) * fr.y,         fr.x * fr.y };
    int2   off[4]   = { int2(0, 0), int2(1, 0), int2(0, 1), int2(1, 1) };

    float3 pColor = float3(0.0);
    float2 pMom   = float2(0.0);
    float  pHist  = 0.0;
    float  sumw   = 0.0;
    // Rougher reflections are blurry -> hitDist-insensitive, so loosen the reflected-depth gate.
    float hitTol = pc.depthThreshold * (1.0 + rough * 4.0);
    bool inBounds = all(prevUV >= float2(0.0)) && all(prevUV <= float2(1.0));
    if (inBounds)
    {
        for (int i = 0; i < 4; ++i)
        {
            int2 tp = p0 + off[i];
            if (any(tp < int2(0)) || any(tp >= size)) continue;

            float4 g        = u_GeomPrev[tp];
            float  prevLinZ = g.r;
            float3 prevN    = SvgfOctDecode(g.gb);
            float  prevHit  = g.a;
            bool ok = abs(curLinZ - prevLinZ) < pc.depthThreshold * curLinZ
                   && dot(prevN, N) > pc.normalThreshold
                   && abs(hitDist - prevHit) < hitTol * max(hitDist, prevHit) + 0.01;
            if (!ok) continue;

            float w = bw[i];
            pColor += w * u_ColorHistPrev[tp].rgb;
            float3 m = u_MomentsPrev[tp].rgb;   // mu1, mu2, histLen
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
        histLen = 1.0;
        color   = di;
        mom     = float2(curLum, curLum * curLum);
    }

    float variance = mom.y - mom.x * mom.x;
    variance = (isinf(variance) || isnan(variance)) ? 0.0 : max(0.0, variance);
    outColor[pixel]   = float4(color, variance);
    outMoments[pixel] = float4(mom, histLen, 0.0);
}
