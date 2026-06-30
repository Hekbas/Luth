// Depth visualization — linearize + normalize + invert (close = bright).
[[vk::binding(0, 0)]] Sampler2D u_DepthTexture;
struct PC { float nearPlane; float farPlane; };
[[vk::push_constant]] PC pc;
struct FIn { [[vk::location(0)]] float2 uv; };

float LinearizeDepth(float d, float near, float far) { return near * far / (far + d * (near - far)); }

[shader("fragment")]
float4 main(FIn i) : SV_Target
{
    float depth = u_DepthTexture.Sample(i.uv).r;
    float linearDepth = LinearizeDepth(depth, pc.nearPlane, pc.farPlane);
    float normalized = 1.0 - clamp(linearDepth / pc.farPlane, 0.0, 1.0);
    return float4(normalized.xxx, 1.0);
}
