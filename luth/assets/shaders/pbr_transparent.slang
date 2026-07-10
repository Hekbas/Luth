// Sorted transparent forward pass: back-to-front, depth-test-no-write, alpha blend onto sceneColor.
// The shaded body lives in pbr_transparent_shading; the PPLL store path consumes the identical helper.
import globals;
import pbr_transparent_shading;

struct VIn   // matches pbr.vert (TBN delivered as separate T/B/N varyings)
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
    [[vk::location(10)]] float4 color;
};
struct FOut
{
    [[vk::location(0)]] float4 color    : SV_Target0;
    [[vk::location(1)]] uint   entityID : SV_Target1;
};

[shader("fragment")]
FOut main(VIn i, bool frontFacing : SV_IsFrontFace, float4 fragCoord : SV_Position)
{
    FOut o;
    o.entityID = i.entityID;
    o.color = EvalTransparentSurfaceColor(i.materialIndex, i.shadeMode, i.uv0, i.uv1,
                                          i.T, i.B, i.N, frontFacing, i.worldPos, fragCoord, i.color);
    return o;
}
