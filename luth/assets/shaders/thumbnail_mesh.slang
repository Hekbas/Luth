// Thumbnail bake fragment — samples the engine-wide bindless array (Set 0 here = the main pipeline's Set 1
// layout); diffuseIndex via push constant (slot 0 = 1x1 white fallback). Simple Lambert preview.
[[vk::binding(0, 0)]] Sampler2D globalTextures[];

struct PC
{
    float4x4 viewProj;
    float4   albedo;
    uint     diffuseIndex;
    float4   emissive;   // rgb = factor, a = HDR strength
};
[[vk::push_constant]] PC pc;

struct FIn
{
    [[vk::location(0)]] float3 vNormal;
    [[vk::location(1)]] float4 vAlbedo;
    [[vk::location(2)]] float2 vUV;
};

[shader("fragment")]
float4 main(FIn i) : SV_Target
{
    float4 sampled = globalTextures[NonUniformResourceIndex(pc.diffuseIndex)].Sample(i.vUV);
    float3 base = sampled.rgb * i.vAlbedo.rgb;

    const float3 L = normalize(float3(0.5, 1.0, 0.7));
    float diffuse = max(dot(normalize(i.vNormal), L), 0.0);
    float3 ambient = base * 0.25;
    float3 emission = pc.emissive.rgb * pc.emissive.a;
    return float4(ambient + diffuse * base * 0.75 + emission, 1.0);
}
