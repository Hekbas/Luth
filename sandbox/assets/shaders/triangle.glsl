#type vertex
#version 450

layout(binding = 0) uniform UniformBufferObject {
    mat4 model;
    mat4 view;
    mat4 proj;
} ubo;

layout(location = 0) in vec2 inPosition;
layout(location = 1) in vec3 inColor;
layout(location = 2) in vec2 inTexCoord;

layout(location = 0) out vec3 fragColor;
layout(location = 1) out vec2 texCoord;

void main() {
    gl_Position = ubo.proj * ubo.view * ubo.model * vec4(inPosition, 0.0, 1.0);
    fragColor = inColor;
    texCoord = inTexCoord;
}


#type fragment
#version 450

layout(location = 0) in vec3 fragColor;
layout(location = 1) in vec2 texCoord;

layout(location = 0) out vec4 outColor;

uniform sampler2D u_Texture;

void main() {
    outColor = texture(u_Texture, texCoord) * vec4(fragColor, 1.0);
}
