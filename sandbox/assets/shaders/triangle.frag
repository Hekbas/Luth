#version 450
#extension GL_EXT_nonuniform_qualifier : enable

layout(location = 0) in vec3 v_Normal;
layout(location = 1) in vec2 v_TexCoord;

layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform GlobalUniforms {
    mat4 viewProjection;
    mat4 view;
    mat4 projection;
    vec3 cameraPos;
    float time;
} ubo;

layout(set = 1, binding = 0) uniform sampler2D globalTextures[];

layout(push_constant) uniform PushConstants {
    mat4 modelMatrix;
    uint albedoMapIndex;
} pc;

void main() {
    vec3 lightDir = normalize(vec3(1.0, 1.0, 1.0));
    float diff = max(dot(normalize(v_Normal), lightDir), 0.1);
    
    // Bindless lookup
    vec4 albedo = texture(globalTextures[nonuniformEXT(pc.albedoMapIndex)], v_TexCoord);
    
    outColor = vec4(albedo.rgb * diff, albedo.a);
}