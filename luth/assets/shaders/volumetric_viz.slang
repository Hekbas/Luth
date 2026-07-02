// Volumetric fog debug viz. Mode 0 = density heat-map (R x scale); mode 1 = in-scatter HDR radiance
// (log10-encoded). Both derive the per-fragment Wronski slice from SceneDepth and read the 3D atlas at
// (screenUV, sliceW). Alpha-blended onto LDR. see arch/rendering-pipeline.md
import globals;

[[vk::binding(0, 1)]] Sampler2D u_SceneDepth;
[[vk::binding(1, 1)]] Sampler3D u_VolDensity;
[[vk::binding(2, 1)]] Sampler3D u_VolInScatter;

struct PC
{
    uint  mode;     // 0 = density, 1 = in-scatter
    float scale;    // user-tunable display multiplier
    float overlayAlpha;
};
[[vk::push_constant]] PC pc;

float3 HeatColor(float t)
{
    t = clamp(t, 0.0, 1.0);
    if (t < 0.33)      return lerp(float3(0.0, 0.2, 0.7), float3(0.0, 0.7, 0.2), smoothstep(0.0, 0.33, t));
    else if (t < 0.66) return lerp(float3(0.0, 0.7, 0.2), float3(0.95, 0.85, 0.1), smoothstep(0.33, 0.66, t));
    else               return lerp(float3(0.95, 0.85, 0.1), float3(0.95, 0.15, 0.05), smoothstep(0.66, 1.0, t));
}

[shader("fragment")]
float4 main([[vk::location(0)]] float2 v_TexCoord, float4 fragCoord : SV_Position) : SV_Target
{
    float ndcDepth = u_SceneDepth.Sample(v_TexCoord).r;
    if (ndcDepth >= 0.9999) return float4(0.0);   // sky -> skip

    // Inverse of glm::perspectiveRH_ZO (Vulkan depth range 0..1).
    float viewZ  = (ubo.nearZ * ubo.farZ) / (ndcDepth * (ubo.nearZ - ubo.farZ) + ubo.farZ);
    float sliceW = clamp(log(max(viewZ, ubo.nearZ) / ubo.nearZ) / log(ubo.farZ / ubo.nearZ), 0.0, 1.0);
    float3 uvw = float3(v_TexCoord, sliceW);

    if (pc.mode == 0u)
    {
        float d = u_VolDensity.Sample(uvw).r * pc.scale;
        return float4(HeatColor(clamp(d, 0.0, 1.0)), pc.overlayAlpha);
    }
    else
    {
        // In-scatter is HDR; log10-encode for diagnostic spread.
        float3 s = u_VolInScatter.Sample(uvw).rgb * pc.scale;
        float3 log_s = log(s + float3(1.0)) * 0.434;  // log10(s+1)
        return float4(clamp(log_s, float3(0.0), float3(1.0)), pc.overlayAlpha);
    }
}
