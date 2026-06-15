#version 460

// Skinned camera-space depth prepass — full vertex stride, skinned LBS.
// Writes SceneDepth ahead of the forward GeometryPass.

layout(location = 0) in vec3  a_Position;
layout(location = 1) in vec3  a_Normal;
layout(location = 2) in vec2  a_TexCoord0;
layout(location = 3) in vec2  a_TexCoord1;
layout(location = 4) in vec3  a_Tangent;
layout(location = 5) in ivec4 a_BoneIDs;
layout(location = 6) in vec4  a_BoneWeights;

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

void main()
{
    GPUObjectData obj = objects[gl_BaseInstance];

    // Linear Blend Skinning (LBS)
    mat4 skinMatrix = mat4(0.0);
    for (int i = 0; i < 4; i++) {
        if (a_BoneIDs[i] >= 0)
            skinMatrix += a_BoneWeights[i] * bones[obj.boneOffset + a_BoneIDs[i]];
    }
    if (skinMatrix[0][0] == 0.0 && skinMatrix[1][1] == 0.0 && skinMatrix[2][2] == 0.0)
        skinMatrix = mat4(1.0);

    vec4 skinnedPos = skinMatrix * vec4(a_Position, 1.0);
    gl_Position = ubo.viewProjection * obj.model * skinnedPos;
}
