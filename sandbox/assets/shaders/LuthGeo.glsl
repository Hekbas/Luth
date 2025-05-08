#type vertex
#version 460 core

layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec3 a_Normal;

layout(std140, binding = 0) uniform TransformUBO {
    mat4 view;
    mat4 projection;
    mat4 model;
};

out vec3 v_WorldPos;
out vec3 v_WorldNormal;

void main() {
    vec4 worldPos = model * vec4(a_Position, 1.0);
    v_WorldPos = worldPos.xyz;
    gl_Position = projection * view * worldPos;
    
    mat3 normalMatrix = transpose(inverse(mat3(model)));
    v_WorldNormal = normalize(normalMatrix * a_Normal);
}




#type fragment
#version 460 core

layout(location = 0) out vec3 gPosition;
layout(location = 1) out vec3 gNormal;

in vec3 v_WorldPos;
in vec3 v_WorldNormal;

void main() {
    gPosition = v_WorldPos;
    gNormal = normalize(v_WorldNormal);
}