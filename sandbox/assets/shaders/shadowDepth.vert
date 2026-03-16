#version 450

// Only position is needed for depth-only pass.
// The full vertex stride is still used (other attributes are present in the buffer but ignored).
layout(location = 0) in vec3 a_Position;

layout(set = 0, binding = 0) uniform GlobalUniforms {
    mat4 viewProjection;
    mat4 view;
    mat4 projection;
    vec3 cameraPos;
    float time;
    mat4 lightSpaceMatrix;
    float shadowBias;
    float _pad[3];
} ubo;

layout(push_constant) uniform PushConstants {
    mat4 model;
    uint materialIndex;
} pc;

void main()
{
    gl_Position = ubo.lightSpaceMatrix * pc.model * vec4(a_Position, 1.0);
}
