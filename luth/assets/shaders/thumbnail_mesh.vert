#version 460

// Thumbnail bake shader. Single sampler binding at set=0 binding=0; mesh bakes
// bind a 1x1 white default (so albedoTex × albedo == albedo), material bakes
// bind the material's albedo VKTexture view/sampler directly (sidesteps the
// bindless-registration race for textures that haven't finished async upload).
//
// Model matrix is implicit identity — the bake camera is fitted to the mesh's
// model-space AABB, so the vertex shader can pass position + normal through.

layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec3 a_Normal;
layout(location = 2) in vec2 a_UV;

layout(push_constant) uniform PC {
    mat4 viewProj;
    vec4 albedo;
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
