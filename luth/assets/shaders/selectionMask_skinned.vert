#version 450

// Selection mask pass (skinned variant) — renders selected skinned entities from camera viewpoint.
layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec3 a_Normal;
layout(location = 2) in vec2 a_TexCoord0;
layout(location = 3) in vec2 a_TexCoord1;
layout(location = 4) in vec3 a_Tangent;
layout(location = 5) in ivec4 a_BoneIDs;
layout(location = 6) in vec4 a_BoneWeights;

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
    if (skinMatrix[0][0] == 0.0 && skinMatrix[1][1] == 0.0 && skinMatrix[2][2] == 0.0)
        skinMatrix = mat4(1.0);

    vec4 skinnedPos = skinMatrix * vec4(a_Position, 1.0);
    gl_Position = ubo.viewProjection * pc.model * skinnedPos;
}
