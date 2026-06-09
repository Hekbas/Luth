#version 460

// Thumbnail bake shader. Samples the engine-wide bindless texture array (Set 0 here,
// same descriptor layout as the main pipeline's Set 1). diffuseIndex is supplied per
// bake as a push constant — slot 0 (1x1 white) when no albedo texture is assigned,
// so x*albedo collapses to the flat tint.
//
// Model matrix is implicit identity — the bake camera is fitted to the mesh's
// model-space AABB, so the vertex shader can pass position + normal through.

layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec3 a_Normal;
layout(location = 2) in vec2 a_UV;

layout(push_constant) uniform PC {
    mat4 viewProj;
    vec4 albedo;
    uint diffuseIndex;
    vec4 emissive;   // rgb = factor (linear), a = HDR strength — read in the frag; block parity only
} pc;

layout(location = 0) out vec3 vNormal;
layout(location = 1) out vec4 vAlbedo;
layout(location = 2) out vec2 vUV;

void main()
{
    gl_Position = pc.viewProj * vec4(a_Position, 1.0);
    vNormal = a_Normal;
    vAlbedo = pc.albedo;
    vUV     = a_UV;
}
