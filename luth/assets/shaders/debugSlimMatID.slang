// Slim G-buffer material-ID preview: reads the R16U integer attachment, emits a per-slot palette.
[[vk::binding(0, 0)]] Sampler2D<uint> u_SourceMatID;
struct FIn { [[vk::location(0)]] float2 uv; };

[shader("fragment")]
float4 main(FIn i) : SV_Target
{
    uint id = u_SourceMatID.Sample(i.uv).r;
    if (id == 0u) return float4(0.0, 0.0, 0.0, 1.0);   // cleared / unmapped slot

    float f = float(id);
    float3 c = float3(frac(f * 0.123), frac(f * 0.456), frac(f * 0.789));
    return float4(c, 1.0);
}
