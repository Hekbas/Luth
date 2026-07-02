// PPLL resolve: walk each pixel's node list, exact-sort the K nearest, order-independently merge the
// tail, under-composite front-to-back. Output (C, T) pairs with blend src=ONE dst=SRC_ALPHA.
import oit_common;

[[vk::binding(0, 0)]] [[vk::image_format("r32ui")]] RWTexture2D<uint> oitHeads;
// OITNodeBuffer = header { uint count; uint3 _pad } (16 B) then OITNode[] (16 B each) at offset 16.
[[vk::binding(1, 0)]] ByteAddressBuffer g_Nodes;

struct PC { uint maxK; };
[[vk::push_constant]] PC pc;

static const uint OIT_MAX_K = 16u;

struct FOut
{
    [[vk::location(0)]] float4 color    : SV_Target0;
    [[vk::location(1)]] uint   entityID : SV_Target1;
};

OITNode LoadNode(uint it)
{
    uint4 n = g_Nodes.Load4(16u + it * 16u);
    OITNode o;
    o.colorRGB9E5 = n.x; o.depthAlpha = n.y; o.entityID = n.z; o.next = n.w;
    return o;
}

[shader("fragment")]
FOut main(float4 fragCoord : SV_Position)
{
    FOut o;
    int2 px   = int2(fragCoord.xy);
    uint head = oitHeads[px];
    if (head == OIT_EMPTY) discard;

    uint  keys  [OIT_MAX_K];
    uint  colors[OIT_MAX_K];
    uint  nearestEntity = 0u;
    uint  kept = 0u;
    float3 tailColor = float3(0.0);
    float  tailTrans = 1.0;

    const uint K = min(pc.maxK, OIT_MAX_K);

    for (uint it = head, guard = 0u; it != OIT_EMPTY && guard < 1024u; ++guard)
    {
        OITNode n = LoadNode(it);
        it = n.next;

        if (kept < K)
        {
            uint i = kept;
            while (i > 0u && keys[i - 1u] > n.depthAlpha)
            {
                keys[i] = keys[i - 1u]; colors[i] = colors[i - 1u];
                --i;
            }
            keys[i] = n.depthAlpha; colors[i] = n.colorRGB9E5;
            if (i == 0u) nearestEntity = n.entityID;
            ++kept;
        }
        else if (n.depthAlpha < keys[K - 1u])
        {
            float ea = OitUnpackAlpha(keys[K - 1u]);
            tailColor += OitUnpackRGB9E5(colors[K - 1u]) * ea;
            tailTrans *= (1.0 - ea);
            uint i = K - 1u;
            while (i > 0u && keys[i - 1u] > n.depthAlpha)
            {
                keys[i] = keys[i - 1u]; colors[i] = colors[i - 1u];
                --i;
            }
            keys[i] = n.depthAlpha; colors[i] = n.colorRGB9E5;
            if (i == 0u) nearestEntity = n.entityID;
        }
        else
        {
            float a = OitUnpackAlpha(n.depthAlpha);
            tailColor += OitUnpackRGB9E5(n.colorRGB9E5) * a;
            tailTrans *= (1.0 - a);
        }
    }

    float3 C = float3(0.0);
    float  T = 1.0;
    for (uint i = 0u; i < kept; ++i)
    {
        float a = OitUnpackAlpha(keys[i]);
        C += T * a * OitUnpackRGB9E5(colors[i]);
        T *= (1.0 - a);
    }
    C += T * tailColor;
    T *= tailTrans;

    o.color    = float4(C, T);
    o.entityID = nearestEntity;
    return o;
}
