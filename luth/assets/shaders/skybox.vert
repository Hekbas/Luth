#version 450

layout(location = 0) in vec3 a_Position;

layout(location = 0) out vec3 v_TexCoord;

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

void main()
{
    v_TexCoord = a_Position;

    // Remove translation from view matrix (skybox stays centered on camera)
    mat4 viewNoTranslation = mat4(mat3(ubo.view));
    vec4 pos = ubo.projection * viewNoTranslation * vec4(a_Position, 1.0);

    // Set z = w so depth = 1.0 after perspective divide (skybox renders behind everything)
    gl_Position = pos.xyww;
}
