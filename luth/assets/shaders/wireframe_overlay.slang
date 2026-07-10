// Flat unlit fragment for the shaded-wireframe edge overlay. The geometry pass redraws opaque + cutout
// meshes in line-polygon mode with this shader over the lit fill (depth-tested LEQUAL, no depth write).
// VIn/FOut mirror pbr_shade so the pbr_vert interface + MRT (SceneColor + entityID) match the fill exactly;
// writes a flat wire color and passes the real entityID through so the entity-id target stays coherent.
struct VIn
{
    [[vk::location(0)]] float3 worldPos;
    [[vk::location(1)]] float3 normal;
    [[vk::location(2)]] float2 uv0;
    [[vk::location(3)]] float3 T;
    [[vk::location(4)]] float3 B;
    [[vk::location(5)]] float3 N;
    [[vk::location(6)]] float2 uv1;
    [[vk::location(7)]] nointerpolation uint materialIndex;
    [[vk::location(8)]] nointerpolation uint shadeMode;
    [[vk::location(9)]] nointerpolation uint entityID;
    [[vk::location(10)]] float4 color;   // unused here; keeps the VIn an exact mirror of the pbr_vert output
};
struct FOut
{
    [[vk::location(0)]] float4 color    : SV_Target0;
    [[vk::location(1)]] uint   entityID : SV_Target1;
};

[shader("fragment")]
FOut main(VIn i)
{
    FOut o;
    o.color    = float4(0.45, 0.85, 1.0, 1.0);   // matches the plain Wireframe line color
    o.entityID = i.entityID;
    return o;
}
