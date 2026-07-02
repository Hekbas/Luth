// Bloom pyramid downsample: Jimenez "Next Gen Post Processing in CoD:AW" 13-tap filter.
// The scene -> mip0 step sets prefilter: bright-pass (soft-knee threshold) + Karis partial
// average over the 5 box groups, which suppresses fireflies at the brightest source. Later
// mip[i] -> mip[i+1] steps run the plain 13-tap. see arch/rendering-pipeline.md (PostProcess).

[[vk::binding(0, 0)]] Sampler2D u_Src;
[[vk::binding(1, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> u_Dst;

struct PC
{
    float2 srcTexel;   // 1 / sourceResolution; tap spacing (source is 2x the dest extent)
    int2   dstSize;    // dest extent, for the dispatch bounds + UV denominator
    float  threshold;  // bright-pass cutoff (prefilter only)
    float  knee;       // soft-knee half-width (prefilter only)
    uint   prefilter;  // 1 -> apply threshold + Karis on the scene -> mip0 step
    uint   _pad;
};
[[vk::push_constant]] PC pc;

static const float3 LUMA = float3(0.2126, 0.7152, 0.0722);

// Karis weight: bright groups contribute less, so one firefly can't dominate the average.
float KarisWeight(float3 c) { return 1.0 / (1.0 + dot(c, LUMA)); }

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (int(tid.x) >= pc.dstSize.x || int(tid.y) >= pc.dstSize.y) return;
    float2 uv = (float2(tid.xy) + 0.5) / float2(pc.dstSize);
    float2 t  = pc.srcTexel;

    // 13 bilinear taps: a..i = outer 3x3 (2-texel spacing), j..m = inner 2x2 (1-texel).
    float3 a = u_Src.SampleLevel(uv + t * float2(-2, -2), 0).rgb;
    float3 b = u_Src.SampleLevel(uv + t * float2( 0, -2), 0).rgb;
    float3 c = u_Src.SampleLevel(uv + t * float2( 2, -2), 0).rgb;
    float3 d = u_Src.SampleLevel(uv + t * float2(-2,  0), 0).rgb;
    float3 e = u_Src.SampleLevel(uv + t * float2( 0,  0), 0).rgb;
    float3 f = u_Src.SampleLevel(uv + t * float2( 2,  0), 0).rgb;
    float3 g = u_Src.SampleLevel(uv + t * float2(-2,  2), 0).rgb;
    float3 h = u_Src.SampleLevel(uv + t * float2( 0,  2), 0).rgb;
    float3 i = u_Src.SampleLevel(uv + t * float2( 2,  2), 0).rgb;
    float3 j = u_Src.SampleLevel(uv + t * float2(-1, -1), 0).rgb;
    float3 k = u_Src.SampleLevel(uv + t * float2( 1, -1), 0).rgb;
    float3 l = u_Src.SampleLevel(uv + t * float2(-1,  1), 0).rgb;
    float3 m = u_Src.SampleLevel(uv + t * float2( 1,  1), 0).rgb;

    float3 result;
    if (pc.prefilter != 0u)
    {
        // 5 box groups (each a 2x2 average), pre-weighted by the CoD kernel (center 0.5, corners
        // 0.125 each), then Karis-weighted to kill fireflies before they pollute the chain.
        float3 g0 = (a + b + d + e) * (0.125 / 4.0);
        float3 g1 = (b + c + e + f) * (0.125 / 4.0);
        float3 g2 = (d + e + g + h) * (0.125 / 4.0);
        float3 g3 = (e + f + h + i) * (0.125 / 4.0);
        float3 g4 = (j + k + l + m) * (0.500 / 4.0);
        result = g0 * KarisWeight(g0) + g1 * KarisWeight(g1) + g2 * KarisWeight(g2)
               + g3 * KarisWeight(g3) + g4 * KarisWeight(g4);

        // Soft-knee bright-pass (Unity recipe): keep only the over-threshold energy, smoothly.
        float br   = dot(result, LUMA);
        float soft = clamp(br - pc.threshold + pc.knee, 0.0, 2.0 * pc.knee);
        soft       = soft * soft / (4.0 * pc.knee + 1e-4);
        result    *= max(soft, br - pc.threshold) / max(br, 1e-4);
        result     = max(result, 0.0);
    }
    else
    {
        // Plain CoD 13-tap: center box 0.5, four outer boxes 0.125 each (weights sum to 1).
        result  = e * 0.125;
        result += (a + c + g + i) * 0.03125;
        result += (b + d + f + h) * 0.0625;
        result += (j + k + l + m) * 0.125;
    }

    u_Dst[tid.xy] = float4(result, 1.0);
}
