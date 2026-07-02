// Skybox fragment: samples the prefiltered env at mip 0 (unfiltered) * skybox intensity.
import globals;

[[vk::binding(2, 0)]] SamplerCube prefilteredMap;

struct FIn { [[vk::location(0)]] float3 texCoord; };

[shader("fragment")]
float4 main(FIn i) : SV_Target
{
    float3 color = prefilteredMap.SampleLevel(i.texCoord, 0.0).rgb * ubo.skyboxIntensity;
    return float4(color, 1.0);
}
