#version 450

layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec3 a_Normal;
layout(location = 2) in vec2 a_TexCoord0;
layout(location = 3) in vec2 a_TexCoord1;
layout(location = 4) in vec3 a_Tangent;
layout(location = 5) in ivec4 a_BoneIDs;
layout(location = 6) in vec4 a_BoneWeights;

layout(location = 0) out vec3 v_WorldPos;
layout(location = 1) out vec3 v_Normal;
layout(location = 2) out vec2 v_TexCoord0;
layout(location = 6) out vec2 v_TexCoord1;
layout(location = 3) out mat3 v_TBN;    // consumes locations 3, 4, 5

// Set 0: Global Uniforms
layout(set = 0, binding = 0) uniform GlobalUniforms {
    mat4 viewProjection;
    mat4 view;
    mat4 projection;
    vec3 cameraPos;
    float time;
    mat4 lightSpaceMatrix;
    float shadowBias;
    float iblIntensity;
    float skyboxIntensity;
    float _pad;
} ubo;

// Set 4: Bone Matrices SSBO
layout(std430, set = 4, binding = 0) readonly buffer BoneMatrices {
    mat4 bones[];
};

// Push Constants
layout(push_constant) uniform PushConstants {
    mat4 model;
    uint materialIndex;
    uint shadeMode;
    uint entityID;
    uint boneOffset;
} pc;

void main()
{
    // Linear Blend Skinning (LBS)
    mat4 skinMatrix = mat4(0.0);
    for (int i = 0; i < 4; i++) {
        if (a_BoneIDs[i] >= 0)
            skinMatrix += a_BoneWeights[i] * bones[pc.boneOffset + a_BoneIDs[i]];
    }
    // Fallback: if no bones influence this vertex, use identity
    if (skinMatrix[0][0] == 0.0 && skinMatrix[1][1] == 0.0 && skinMatrix[2][2] == 0.0)
        skinMatrix = mat4(1.0);

    // Apply skinning to position, normal, and tangent
    vec4 skinnedPos = skinMatrix * vec4(a_Position, 1.0);
    mat3 skinNormalMat = mat3(skinMatrix);
    vec3 skinnedNormal = normalize(skinNormalMat * a_Normal);
    vec3 skinnedTangent = normalize(skinNormalMat * a_Tangent);

    // World transform
    vec4 worldPos = pc.model * skinnedPos;
    v_WorldPos = worldPos.xyz;
    v_TexCoord0 = a_TexCoord0;
    v_TexCoord1 = a_TexCoord1;

    // Normal matrix (handles non-uniform scale)
    mat3 normalMatrix = mat3(transpose(inverse(pc.model)));
    v_Normal = normalize(normalMatrix * skinnedNormal);

    // TBN matrix for normal mapping (Gram-Schmidt re-orthogonalization)
    vec3 T = normalize(mat3(pc.model) * skinnedTangent);
    vec3 N = v_Normal;
    T = normalize(T - dot(T, N) * N); // Re-orthogonalize
    vec3 B = cross(N, T);
    v_TBN = mat3(T, B, N);

    gl_Position = ubo.viewProjection * worldPos;
}
