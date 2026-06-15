#version 460
#extension GL_EXT_buffer_reference        : require
#extension GL_EXT_buffer_reference_uvec2  : require

layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec3 a_Normal;
layout(location = 2) in vec2 a_TexCoord0;
layout(location = 3) in vec2 a_TexCoord1;
layout(location = 4) in vec3 a_Tangent;
layout(location = 5) in ivec4 a_BoneIDs;
layout(location = 6) in vec4 a_BoneWeights;

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

// Set 4: Bone Matrices SSBO
layout(std430, set = 4, binding = 0) readonly buffer BoneMatrices {
    mat4 bones[];
};

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
    uint  prevBoneOffset; // 4B — prev-frame bones region (dual-region BoneMatrixBuffer)
    uvec2 deformedBdaCurr;
    uvec2 deformedBdaPrev;
};

layout(std430, set = 5, binding = 0) readonly buffer ObjectBuffer {
    GPUObjectData objects[];
};

// Deformation seam — post-skin object-space position (interleaved Vertex, 13 floats/vert).
layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer DeformedBuf {
    float verts[];
};

// CPU pushes the cascade index per ShadowPass.Ci invocation.
layout(push_constant) uniform PushConstants {
    uint cascadeIndex;
} pc;

void main()
{
    GPUObjectData obj = objects[gl_BaseInstance];

    // Guard: deformed buffer not ready (skinned BLAS still loading) — clip off-screen, no null deref.
    if (obj.deformedBdaCurr == uvec2(0u)) { gl_Position = vec4(0.0, 0.0, 2.0, 1.0); return; }

    DeformedBuf db = DeformedBuf(obj.deformedBdaCurr);
    uint b = uint(gl_VertexIndex) * 13u;
    vec3 dPos = vec3(db.verts[b + 0u], db.verts[b + 1u], db.verts[b + 2u]);

    gl_Position = ubo.lightSpaceMatrix[pc.cascadeIndex] * obj.model * vec4(dPos, 1.0);
}
