// Thumbnail bake vertex. Model matrix implicit identity (bake camera fitted to the model-space AABB), so
// position + normal pass through. albedo/diffuseIndex/emissive ride the PC for the frag (block parity).
struct VSIn
{
    [[vk::location(0)]] float3 position;
    [[vk::location(1)]] float3 normal;
    [[vk::location(2)]] float2 uv;
};
struct PC
{
    float4x4 viewProj;
    float4   albedo;
    uint     diffuseIndex;
    float4   emissive;   // rgb = factor, a = HDR strength; read in the frag; block parity only
};
[[vk::push_constant]] PC pc;

struct VOut
{
    float4 clipPos : SV_Position;
    [[vk::location(0)]] float3 vNormal;
    [[vk::location(1)]] float4 vAlbedo;
    [[vk::location(2)]] float2 vUV;
};

[shader("vertex")]
VOut main(VSIn vin)
{
    VOut o;
    o.clipPos = mul(pc.viewProj, float4(vin.position, 1.0));
    o.vNormal = vin.normal;
    o.vAlbedo = pc.albedo;
    o.vUV     = vin.uv;
    return o;
}
