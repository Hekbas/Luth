#type vertex
#version 460 core

layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec3 a_Normal;

layout(location = 0) out vec3 v_WorldPos;
layout(location = 1) out vec3 v_WorldNormal;

layout(std140, binding = 0) uniform TransformUBO {
    mat4 view;
    mat4 projection;
    mat4 model;
};

void main()
{
    v_WorldPos = vec3(model * vec4(a_Position, 1.0));
    gl_Position = projection * view * vec4(v_WorldPos, 1.0);
    
    mat3 normalMatrix = transpose(inverse(mat3(model)));
    v_WorldNormal = normalize(normalMatrix * a_Normal);
}




#type fragment
#version 460 core

layout(location = 0) in vec3 v_WorldPos;
layout(location = 1) in vec3 v_WorldNormal;

layout(location = 0) out vec3 gPosition;
layout(location = 1) out vec3 gNormal;

void main() {
    gPosition = v_WorldPos;
    gNormal = normalize(v_WorldNormal);
}