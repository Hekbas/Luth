#version 450
layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec3 a_Normal;
layout(location = 2) in vec2 a_TexCoord0;
layout(location = 3) in vec2 a_TexCoord1;
layout(location = 4) in vec3 a_Tangent;

layout(location = 0) out vec3 v_Normal;
layout(location = 1) out vec2 v_TexCoord;

layout(set = 0, binding = 0) uniform GlobalUniforms {
    mat4 viewProjection;
    mat4 view;
    mat4 projection;
    vec3 cameraPos;
    float time;
} ubo;

layout(push_constant) uniform PushConstants {
    mat4 modelMatrix;
    uint albedoMapIndex;
} pc;

void main() {
    v_Normal = mat3(transpose(inverse(pc.modelMatrix))) * a_Normal;
    v_TexCoord = a_TexCoord0;
    gl_Position = ubo.viewProjection * pc.modelMatrix * vec4(a_Position, 1.0);
}