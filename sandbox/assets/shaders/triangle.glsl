#type vertex
#version 450

layout(binding = 0) uniform UniformBufferObject {
    mat4 model;
    mat4 view;
    mat4 proj;
} ubo;

layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec3 a_Normal;
layout(location = 2) in vec2 a_TexCoord;

layout(location = 0) out vec3 v_Normal;
layout(location = 1) out vec2 v_TexCoord;

void main() {
    gl_Position = ubo.proj * ubo.view * ubo.model * vec4(a_Position, 1.0);
    v_Normal = a_Normal;
    v_TexCoord = a_TexCoord;
}


#type fragment
#version 450

layout(location = 0) in vec3 v_Normal;
layout(location = 1) in vec2 v_TexCoord;

layout(location = 0) out vec4 FragColor;

uniform sampler2D u_TexDiffuse;
uniform sampler2D u_TexNormal;
uniform sampler2D u_TexRough;

void main() {
    //FragColor = texture(u_TexDiffuse, v_TexCoord);
    FragColor = texture(u_TexNormal, v_TexCoord);
    //FragColor = texture(u_TexRough, v_TexCoord);
}
