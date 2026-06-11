#version 460
#extension GL_EXT_nonuniform_qualifier : enable
#extension GL_GOOGLE_include_directive : enable
#extension GL_EXT_ray_query            : require
#extension GL_EXT_buffer_reference     : require
#extension GL_EXT_buffer_reference2    : require

// PPLL store — shades each transparent fragment once (the shared transparent body) and pushes the
// result onto the pixel's linked list. No color attachments; depth-test-no-write against the
// opaque depth. oit_resolve.frag sorts + composites in the next pass.

#include "common/globals.glsl"
#include "common/pbr_transparent_shading.glsl"
#include "common/oit_common.glsl"

// Run the depth test BEFORE the shader so occluded fragments never shade or store. Legal alongside
// the alpha-cutoff discard because this pipeline never writes depth (the early-test caveat is about
// depth WRITES persisting past a discard).
layout(early_fragment_tests) in;

layout(location = 0) in vec3 v_WorldPos;
layout(location = 1) in vec3 v_Normal;
layout(location = 2) in vec2 v_TexCoord0;
layout(location = 6) in vec2 v_TexCoord1;
layout(location = 3) in mat3 v_TBN;    // locations 3, 4, 5
layout(location = 7) flat in uint v_MaterialIndex;
layout(location = 8) flat in uint v_ShadeMode;
layout(location = 9) flat in uint v_EntityID;

// Set 6 b1/b2 — the PPLL store targets (b0 fog atlas comes from the shared include).
layout(set = 6, binding = 1, r32ui) uniform uimage2D oitHeads;
layout(std430, set = 6, binding = 2) buffer OITNodeBuffer {
    uint    count;
    uint    _pad0, _pad1, _pad2;
    OITNode nodes[];
} oitNodes;

void main()
{
    vec4 c = EvalTransparentSurfaceColor(v_MaterialIndex, v_ShadeMode,
                                         v_TexCoord0, v_TexCoord1, v_TBN, v_Normal,
                                         gl_FrontFacing, v_WorldPos, gl_FragCoord);

    uint idx = atomicAdd(oitNodes.count, 1u);
    if (idx >= pc.nodeCapacity)
        return;   // pool exhausted — fragment dropped (budget knob in the Render panel)

    // Publish-order note: other invocations only ever FOLLOW this node after the resolve pass,
    // which the render graph fences behind every store (no same-pass reader exists).
    uint prev = imageAtomicExchange(oitHeads, ivec2(gl_FragCoord.xy), idx);
    oitNodes.nodes[idx].colorRGB9E5 = OitPackRGB9E5(c.rgb);
    oitNodes.nodes[idx].depthAlpha  = OitPackDepthAlpha(gl_FragCoord.z, c.a);
    oitNodes.nodes[idx].entityID    = v_EntityID;
    oitNodes.nodes[idx].next        = prev;
}
