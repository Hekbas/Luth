#version 450

// Selection mask pass — renders selected entities from camera viewpoint.
// Only position is needed; other attributes are present in the vertex buffer but ignored.
layout(location = 0) in vec3 a_Position;

layout(set = 0, binding = 0) uniform GlobalUniforms {
    mat4 viewProjection;
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
} ubo;

layout(push_constant) uniform PushConstants {
    mat4 model;
    uint materialIndex;
    uint shadeMode;
    uint entityID;
    uint boneOffset;
} pc;

void main()
{
    gl_Position = ubo.viewProjection * pc.model * vec4(a_Position, 1.0);
}
