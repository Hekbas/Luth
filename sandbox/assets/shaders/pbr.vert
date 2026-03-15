#version 450

layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec3 a_Normal;
layout(location = 2) in vec2 a_TexCoord0;
layout(location = 3) in vec2 a_TexCoord1;
layout(location = 4) in vec3 a_Tangent;

layout(location = 0) out vec3 v_WorldPos;
layout(location = 1) out vec3 v_Normal;
layout(location = 2) out vec2 v_TexCoord;
layout(location = 3) out mat3 v_TBN;    // consumes locations 3, 4, 5

// Set 0: Global Uniforms
layout(set = 0, binding = 0) uniform GlobalUniforms {
    mat4 viewProjection;
    mat4 view;
    mat4 projection;
    vec3 cameraPos;
    float time;
    mat4 lightSpaceMatrix;
} ubo;

// Push Constants
layout(push_constant) uniform PushConstants {
    mat4 model;
    uint materialIndex;
} pc;

void main()
{
    vec4 worldPos = pc.model * vec4(a_Position, 1.0);
    v_WorldPos = worldPos.xyz;
    v_TexCoord = a_TexCoord0;

    // Normal matrix (handles non-uniform scale)
    mat3 normalMatrix = mat3(transpose(inverse(pc.model)));
    v_Normal = normalize(normalMatrix * a_Normal);

    // TBN matrix for normal mapping (Gram-Schmidt re-orthogonalization)
    vec3 T = normalize(mat3(pc.model) * a_Tangent);
    vec3 N = v_Normal;
    T = normalize(T - dot(T, N) * N); // Re-orthogonalize
    vec3 B = cross(N, T);
    v_TBN = mat3(T, B, N);

    gl_Position = ubo.viewProjection * worldPos;
}
