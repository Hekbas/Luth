#version 460

// Depth-only pass. Only position is read; other attributes are in the buffer but ignored.
layout(location = 0) in vec3 a_Position;

// Set 0: Global Uniforms
layout(set = 0, binding = 0) uniform GlobalUniforms {
    mat4 viewProjection;
    mat4 prevViewProjection;  // frame N-1's VP — motion vectors + TAA reprojection
    mat4 view;
    mat4 projection;
    vec3 cameraPos;
    float time;
    mat4 lightSpaceMatrix[4];
    vec4 cascadeSplitsViewZ;
    vec4 shadowBias;
    vec4 shadowNormalBias;
    vec4 cascadeTexelSize;
    float iblIntensity;
    float skyboxIntensity;
    float debugVisualizeCascades;
    float cascadeBlendWidth;
    vec2  viewportSize;       // pixels — cluster ID + screen-space recon
    float nearZ;
    float farZ;
} ubo;

// Set 5: Per-object data SSBO (std430, 176 bytes per entry)
struct GPUObjectData {
    mat4  model;          // 64B
    mat4  prevModel;      // 64B — frame N-1's worldMatrix
    vec4  boundingSphere; // 16B
    uint  materialIndex;  // 4B
    uint  shadeMode;      // 4B
    uint  entityID;       // 4B
    uint  boneOffset;     // 4B
    uint  indexCount;     // 4B
    uint  firstIndex;     // 4B
    int   vertexOffset;   // 4B
    uint  prevBoneOffset; // 4B — wired by commit 2 (dual-buffer bones)
};

layout(std430, set = 5, binding = 0) readonly buffer ObjectBuffer {
    GPUObjectData objects[];
};

// Phase 13C: CPU pushes the cascade index per ShadowPass.Ci invocation.
layout(push_constant) uniform PushConstants {
    uint cascadeIndex;
} pc;

void main()
{
    GPUObjectData obj = objects[gl_BaseInstance];
    gl_Position = ubo.lightSpaceMatrix[pc.cascadeIndex] * obj.model * vec4(a_Position, 1.0);
}
