#version 450
#extension GL_GOOGLE_include_directive : enable

// PPLL resolve — walks each pixel's node list, exact-sorts the K nearest (insertion sort on the
// packed depthAlpha key), order-independently merges anything deeper into a tail slab, then
// under-composites front-to-back. Output (C, T) pairs with blend src=ONE dst=SRC_ALPHA:
//   final = C + background * T
// The EntityID attachment receives the nearest node's entity (integer format — blend auto-off);
// `discard` on empty pixels keeps both attachments' loaded contents.

#include "common/oit_common.glsl"

layout(location = 0) in vec2 v_TexCoord;
layout(location = 0) out vec4 outColor;
layout(location = 1) out uint outEntityID;

layout(set = 0, binding = 0, r32ui) uniform readonly uimage2D oitHeads;
layout(std430, set = 0, binding = 1) readonly buffer OITNodeBuffer {
    uint    count;
    uint    _pad0, _pad1, _pad2;
    OITNode nodes[];
} oitNodes;

layout(push_constant) uniform ResolvePC {
    uint maxK;   // exact-sorted layers per pixel (clamped to OIT_MAX_K)
} pc;

#define OIT_MAX_K 16u

void main()
{
    ivec2 px  = ivec2(gl_FragCoord.xy);
    uint head = imageLoad(oitHeads, px).r;
    if (head == OIT_EMPTY)
        discard;

    uint keys  [OIT_MAX_K];
    uint colors[OIT_MAX_K];
    uint nearestEntity = 0u;
    uint kept = 0u;
    vec3  tailColor = vec3(0.0);   // premultiplied radiance of merged-deeper fragments
    float tailTrans = 1.0;

    const uint K = min(pc.maxK, OIT_MAX_K);

    // 1024-iteration guard: a healthy list can't exceed the pool capacity, but a corrupt next
    // pointer must not TDR the device.
    for (uint it = head, guard = 0u; it != OIT_EMPTY && guard < 1024u; ++guard)
    {
        OITNode n = oitNodes.nodes[it];
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
            // Evict the farthest kept slot into the tail, insert the nearer node.
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

    // Front-to-back under-compositing of the exact-sorted layers, then the tail slab behind them.
    vec3  C = vec3(0.0);
    float T = 1.0;
    for (uint i = 0u; i < kept; ++i)
    {
        float a = OitUnpackAlpha(keys[i]);
        C += T * a * OitUnpackRGB9E5(colors[i]);
        T *= (1.0 - a);
    }
    C += T * tailColor;
    T *= tailTrans;

    outColor    = vec4(C, T);
    outEntityID = nearestEntity;
}
