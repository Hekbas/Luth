#type vertex
#version 460 core

layout(location = 0) in vec3 a_Position;

layout(std140, binding = 0) uniform TransformUBO {
    mat4 view;
    mat4 projection;
    mat4 model;
};

const int MAX_BONES = 128;
layout(std140, binding = 2) uniform BonesUBO {
    mat4 u_BoneMatrices[MAX_BONES];
};

// Bone debug data
struct BoneDebug {
    vec3 startPos;      // Bone start (parent position)
    vec3 endPos;        // Bone end (child position)
    int parentIndex;    // -1 for root bones
};
layout(std430, binding = 3) readonly buffer BoneDebugSSBO {
    BoneDebug u_BonesDebug[];
};

out vec3 f_Color;

void main() {
    // Get the bone's start/end in world space
    vec3 worldStart = u_BonesDebug[gl_VertexID].startPos;
    vec3 worldEnd   = u_BonesDebug[gl_VertexID].endPos;

    // Alternate color per bone
    f_Color = vec3(float(gl_VertexID % 3) / 3.0, 
                   float(gl_VertexID % 5) / 5.0, 
                   float(gl_VertexID % 7) / 7.0);

    // Output position (draw as a line)
    gl_Position = projection * view * vec4(
        (gl_VertexID % 2 == 0) ? worldStart : worldEnd, 
        1.0
    );
}




#type fragment
#version 460 core

in vec3 f_Color;
out vec4 o_Color;

void main() {
    o_Color = vec4(f_Color, 1.0);
}