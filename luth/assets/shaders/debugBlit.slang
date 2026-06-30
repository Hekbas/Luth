// HDR -> LDR blit (Reinhard + gamma) for debug display.
[[vk::binding(0, 0)]] Sampler2D u_InputTexture;
struct FIn { [[vk::location(0)]] float2 uv; };

[shader("fragment")]
float4 main(FIn i) : SV_Target
{
    float3 hdr = u_InputTexture.Sample(i.uv).rgb;
    float3 ldr = hdr / (hdr + float3(1.0));
    ldr = pow(ldr, float3(1.0 / 2.2));
    return float4(ldr, 1.0);
}
