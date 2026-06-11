#version 460
#extension GL_EXT_nonuniform_qualifier : enable
#extension GL_GOOGLE_include_directive : enable
#extension GL_EXT_ray_query            : require
#extension GL_EXT_buffer_reference     : require
#extension GL_EXT_buffer_reference2    : require

// Sorted transparent forward pass — runs after skybox + volumetric composite, back-to-front,
// depth-test-no-write, standard alpha blend onto sceneColor. The corrected-input shading
// (rayQuery sun shadow, cluster lights, fragment-depth fog) lives in the shared include; the
// PPLL store path (pbr_oit_store.frag) consumes the identical body.

#include "common/globals.glsl"
#include "common/pbr_transparent_shading.glsl"

layout(location = 0) in vec3 v_WorldPos;
layout(location = 1) in vec3 v_Normal;
layout(location = 2) in vec2 v_TexCoord0;
layout(location = 6) in vec2 v_TexCoord1;
layout(location = 3) in mat3 v_TBN;    // locations 3, 4, 5
layout(location = 7) flat in uint v_MaterialIndex;
layout(location = 8) flat in uint v_ShadeMode;
layout(location = 9) flat in uint v_EntityID;

layout(location = 0) out vec4 outColor;
layout(location = 1) out uint outEntityID;

void main()
{
    outEntityID = v_EntityID;
    outColor = EvalTransparentSurfaceColor(v_MaterialIndex, v_ShadeMode,
                                           v_TexCoord0, v_TexCoord1, v_TBN, v_Normal,
                                           gl_FrontFacing, v_WorldPos, gl_FragCoord);
}
