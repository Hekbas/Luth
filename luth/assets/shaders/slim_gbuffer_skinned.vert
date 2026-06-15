#version 460

// Slim G-buffer vertex shader (skinned). Computes current AND previous clip positions
// from CURRENT model + CURRENT bones, vs PREVIOUS model + PREVIOUS bones. BoneMatrices
// SSBO is dual-region (current at [0,N), previous at [N,2N)) so a single Set 4 binding
// serves both clips — see arch/rendering-pipeline.md (BoneMatrixBuffer dual-buffer).

layout(location = 0) in vec3  a_Position;
layout(location = 1) in vec3  a_Normal;
layout(location = 2) in vec2  a_TexCoord0;
layout(location = 3) in vec2  a_TexCoord1;
layout(location = 4) in vec3  a_Tangent;
layout(location = 5) in ivec4 a_BoneIDs;
layout(location = 6) in vec4  a_BoneWeights;

layout(location = 0) out vec2 v_TexCoord0;
layout(location = 1) out vec2 v_TexCoord1;
layout(location = 2) out mat3 v_TBN;    // consumes locations 2, 3, 4
layout(location = 5) out vec4 v_CurrClip;
layout(location = 6) out vec4 v_PrevClip;
layout(location = 7) flat out uint v_MaterialIndex;

layout(set = 0, binding = 0) uniform GlobalUniforms {
    mat4 viewProjection;
    mat4 prevViewProjection;
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

// Set 4: dual-region Bone Matrices SSBO. Current bones live at [0, PREV_OFFSET);
// previous bones live at [PREV_OFFSET, 2*PREV_OFFSET). GPUObjectData::prevBoneOffset
// is precomputed as boneOffset + PREV_OFFSET — see BoneMatrixBuffer::PREV_BLOCK_OFFSET.
layout(std430, set = 4, binding = 0) readonly buffer BoneMatrices {
    mat4 bones[];
};

// Set 5: Per-object data SSBO (std430, 176 bytes per entry)
struct GPUObjectData {
    mat4  model;
    mat4  prevModel;
    vec4  boundingSphere;
    uint  materialIndex;
    uint  shadeMode;
    uint  entityID;
    uint  boneOffset;
    uint  indexCount;
    uint  firstIndex;
    int   vertexOffset;
    uint  prevBoneOffset;
    uvec2 deformedBdaCurr;
    uvec2 deformedBdaPrev;
};

layout(std430, set = 5, binding = 0) readonly buffer ObjectBuffer {
    GPUObjectData objects[];
};

// Linear Blend Skinning against a chosen bone offset (current = boneOffset,
// previous = prevBoneOffset). Identity fallback matches pbr_skinned.vert.
mat4 SkinAt(uint baseOffset)
{
    mat4 m = mat4(0.0);
    for (int i = 0; i < 4; ++i)
    {
        if (a_BoneIDs[i] >= 0)
            m += a_BoneWeights[i] * bones[baseOffset + a_BoneIDs[i]];
    }
    if (m[0][0] == 0.0 && m[1][1] == 0.0 && m[2][2] == 0.0)
        m = mat4(1.0);
    return m;
}

void main()
{
    GPUObjectData obj = objects[gl_BaseInstance];

    mat4 skinCurr = SkinAt(obj.boneOffset);
    mat4 skinPrev = SkinAt(obj.prevBoneOffset);

    vec4 modelPos     = vec4(a_Position, 1.0);
    vec4 skinnedCurr  = skinCurr * modelPos;
    vec4 skinnedPrev  = skinPrev * modelPos;

    v_TexCoord0 = a_TexCoord0;
    v_TexCoord1 = a_TexCoord1;

    // TBN built against the current skin matrix (slim_gbuffer.frag samples the current
    // normal map; previous-frame TBN isn't needed for motion vectors).
    mat3 skinNorm  = mat3(skinCurr);
    vec3 skinNorm3 = normalize(skinNorm * a_Normal);
    vec3 skinTan3  = normalize(skinNorm * a_Tangent);

    mat3 normalMatrix = mat3(transpose(inverse(obj.model)));
    vec3 N = normalize(normalMatrix * skinNorm3);
    vec3 T = normalize(mat3(obj.model) * skinTan3);
    T = normalize(T - dot(T, N) * N);
    vec3 B = cross(N, T);
    v_TBN = mat3(T, B, N);

    v_MaterialIndex = obj.materialIndex;

    v_CurrClip  = ubo.viewProjection     * obj.model     * skinnedCurr;
    v_PrevClip  = ubo.prevViewProjection * obj.prevModel * skinnedPrev;
    gl_Position = v_CurrClip;
}
