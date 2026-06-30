// Per-vertex debug line endpoint. World-space position; RGBA8 colour (auto-normalized). viewProj via PC.
struct VSIn
{
    [[vk::location(0)]] float3 position;
    [[vk::location(1)]] float4 color;
};
struct PC { float4x4 viewProj; };
[[vk::push_constant]] PC pc;

struct VOut
{
    float4 clipPos : SV_Position;
    [[vk::location(0)]] float4 color;
};

[shader("vertex")]
VOut main(VSIn vin)
{
    VOut o;
    o.clipPos = mul(pc.viewProj, float4(vin.position, 1.0));
    o.color   = vin.color;
    return o;
}
