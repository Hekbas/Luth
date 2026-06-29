// Bloom pyramid upsample — CoD:AW 3x3 tent filter with a scatter radius, accumulated additively
// back up the chain. Tent-filters the smaller source mip and adds it onto the larger dest mip
// in place (the dest already holds its downsample content). see arch/rendering-pipeline.md.

[[vk::binding(0, 0)]] Sampler2D u_Src;   // smaller mip (one level down)
[[vk::binding(1, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> u_Dst;   // larger mip, read-modify-write

struct PC
{
    float2 srcTexel;   // 1 / sourceResolution — tent spacing (source is half the dest extent)
    int2   dstSize;    // dest extent, for the dispatch bounds + UV denominator
    float  radius;     // scatter: scales the tent spread (wider -> softer, broader halo)
    float  _pad0, _pad1, _pad2;
};
[[vk::push_constant]] PC pc;

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (int(tid.x) >= pc.dstSize.x || int(tid.y) >= pc.dstSize.y) return;
    float2 uv = (float2(tid.xy) + 0.5) / float2(pc.dstSize);
    float2 o  = pc.srcTexel * pc.radius;

    // 9-tap tent (1 2 1 / 2 4 2 / 1 2 1, normalized by 1/16).
    float3 s;
    s  = u_Src.SampleLevel(uv + float2(-o.x, -o.y), 0).rgb;
    s += u_Src.SampleLevel(uv + float2( 0.0, -o.y), 0).rgb * 2.0;
    s += u_Src.SampleLevel(uv + float2( o.x, -o.y), 0).rgb;
    s += u_Src.SampleLevel(uv + float2(-o.x,  0.0), 0).rgb * 2.0;
    s += u_Src.SampleLevel(uv,                      0).rgb * 4.0;
    s += u_Src.SampleLevel(uv + float2( o.x,  0.0), 0).rgb * 2.0;
    s += u_Src.SampleLevel(uv + float2(-o.x,  o.y), 0).rgb;
    s += u_Src.SampleLevel(uv + float2( 0.0,  o.y), 0).rgb * 2.0;
    s += u_Src.SampleLevel(uv + float2( o.x,  o.y), 0).rgb;
    s *= (1.0 / 16.0);

    float3 base = u_Dst[tid.xy].rgb;
    u_Dst[tid.xy] = float4(base + s, 1.0);
}
