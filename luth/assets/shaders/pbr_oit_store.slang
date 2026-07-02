// PPLL store: shades each transparent fragment once (the shared body) and pushes the result onto the
// pixel's linked list. No color attachments; depth-test-no-write against the opaque depth. oit_resolve
// sorts + composites next. see arch/rendering-pipeline.md
import globals;
import pbr_transparent_shading;
import oit_common;

// Set 6 b1/b2: PPLL store targets (b0 fog atlas is in the shared module).
[[vk::binding(1, 6)]] [[vk::image_format("r32ui")]] RWTexture2D<uint> oitHeads;
[[vk::binding(2, 6)]] RWByteAddressBuffer oitNodes;   // { uint count; uint pad[3]; OITNode nodes[]; }

struct VIn   // matches pbr.vert
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
};

// Depth test BEFORE the shader so occluded fragments never shade or store (legal; no depth writes here).
[shader("fragment")]
[earlydepthstencil]
void main(VIn i, bool frontFacing : SV_IsFrontFace, float4 fragCoord : SV_Position)
{
    float4 c = EvalTransparentSurfaceColor(i.materialIndex, i.shadeMode, i.uv0, i.uv1,
                                           i.T, i.B, i.N, frontFacing, i.worldPos, fragCoord);

    uint idx;
    oitNodes.InterlockedAdd(0u, 1u, idx);   // atomic count++ at byte offset 0
    if (idx >= pc.nodeCapacity) return;      // pool exhausted: fragment dropped

    uint prev;
    InterlockedExchange(oitHeads[int2(fragCoord.xy)], idx, prev);   // publish as the new head
    uint base = 16u + idx * 16u;                                    // header = {count, pad[3]} = 16 B
    oitNodes.Store(base + 0u,  OitPackRGB9E5(c.rgb));
    oitNodes.Store(base + 4u,  OitPackDepthAlpha(fragCoord.z, c.a));
    oitNodes.Store(base + 8u,  i.entityID);
    oitNodes.Store(base + 12u, prev);
}
