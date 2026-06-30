// Graph-aware material-preview vertex stage. Delivers tangent + UV1 the bounded decode + graphed normal
// need. Per-draw node-world matrix folded into viewProj on the CPU. Out locations match
// material_bindings_preview's PreviewVaryings.
struct VSIn
{
    [[vk::location(0)]] float3 position;
    [[vk::location(1)]] float3 normal;
    [[vk::location(2)]] float2 uv0;
    [[vk::location(3)]] float2 uv1;
    [[vk::location(4)]] float3 tangent;
};
struct PC { float4x4 viewProj; };
[[vk::push_constant]] PC pc;

struct VOut
{
    float4 clipPos : SV_Position;
    [[vk::location(0)]] float3 vNormal;
    [[vk::location(1)]] float3 vTangent;
    [[vk::location(2)]] float2 vUV0;
    [[vk::location(3)]] float2 vUV1;
};

[shader("vertex")]
VOut main(VSIn vin)
{
    VOut o;
    o.clipPos  = mul(pc.viewProj, float4(vin.position, 1.0));
    o.vNormal  = vin.normal;
    o.vTangent = vin.tangent;
    o.vUV0     = vin.uv0;
    o.vUV1     = vin.uv1;
    return o;
}
