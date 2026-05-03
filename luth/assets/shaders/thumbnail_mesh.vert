#version 460

// Thumbnail bake — minimal mesh shader. No descriptor sets; ThumbnailPreviewScene
// drives the pipeline via push constants exclusively, so a bake doesn't require
// any of the engine's per-frame buffers (camera UBO, lights, IBL, GPUObjectData).

layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec3 a_Normal;

layout(push_constant) uniform PC {
    mat4 viewProj;
    mat4 model;
} pc;

layout(location = 0) out vec3 vNormalWS;

void main()
{
    gl_Position = pc.viewProj * pc.model * vec4(a_Position, 1.0);
    // invariant: thumbnails fit a rigid camera-orbit transform — no non-uniform
    // scale — so mat3(model) is sufficient for the normal transform.
    vNormalWS = mat3(pc.model) * a_Normal;
}
