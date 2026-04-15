#version 450

layout(location = 0) in vec3 v_TexCoord;

layout(location = 0) out vec4 outColor;

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
    float iblIntensity;
    float skyboxIntensity;
    float debugVisualizeCascades;
    float _pad;
} ubo;

// Pre-filtered env map at mip 0 = unfiltered environment
layout(set = 0, binding = 2) uniform samplerCube prefilteredMap;

void main()
{
    vec3 color = textureLod(prefilteredMap, v_TexCoord, 0.0).rgb * ubo.skyboxIntensity;
    outColor = vec4(color, 1.0);
}
