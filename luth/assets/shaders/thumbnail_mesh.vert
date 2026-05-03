#version 460

// Thumbnail bake — minimal mesh shader. No descriptor sets; ThumbnailPreviewScene
// drives the pipeline via push constants exclusively, so a bake doesn't require
// any of the engine's per-frame buffers (camera UBO, lights, IBL, GPUObjectData).
//
// Model matrix is implicit identity — the bake camera is fitted to the mesh's
// model-space AABB directly, so the vertex shader can skip the model transform
// and pass normals through unmodified.

layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec3 a_Normal;

layout(push_constant) uniform PC {
    mat4 viewProj;
    vec4 albedo;
} pc;

layout(location = 0) out vec3 vNormal;
layout(location = 1) out vec4 vAlbedo;

void main()
{
    gl_Position = pc.viewProj * vec4(a_Position, 1.0);
    vNormal = a_Normal;
    vAlbedo = pc.albedo;
}
