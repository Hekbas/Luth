#version 460

// Slim G-buffer vertex shader (non-skinned). Pairs with slim_gbuffer.frag to write
// normal (RG16F octahedral) + roughness (R8) + motion vectors (RG16F NDC delta) +
// material ID (R16U) at full viewport resolution. Reuses the PBR vertex layout but
// emits both current and previous clip-space positions so the frag computes motion.

layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec3 a_Normal;
layout(location = 2) in vec2 a_TexCoord0;
layout(location = 3) in vec2 a_TexCoord1;
layout(location = 4) in vec3 a_Tangent;

layout(location = 0) out vec2 v_TexCoord0;
layout(location = 1) out vec2 v_TexCoord1;
layout(location = 2) out mat3 v_TBN;    // consumes locations 2, 3, 4
layout(location = 5) out vec4 v_CurrClip;
layout(location = 6) out vec4 v_PrevClip;
layout(location = 7) flat out uint v_MaterialIndex;

layout(set = 0, binding = 0) uniform GlobalUniforms {
    mat4 viewProjection;
    mat4 prevViewProjection;
    mat4 view;
    mat4 projection;
    vec3 cameraPos;
    float time;
    mat4 lightSpaceMatrix[4];
    vec4 cascadeSplitsViewZ;
    vec4 shadowBias;
    vec4 shadowNormalBias;
    vec4 cascadeTexelSize;
    float iblIntensity;
    float skyboxIntensity;
    float debugVisualizeCascades;
    float cascadeBlendWidth;
    vec2  viewportSize;       // pixels — cluster ID + screen-space recon
    float nearZ;
    float farZ;
} ubo;

// Set 5: Per-object data SSBO (std430, 176 bytes per entry)
struct GPUObjectData {
    mat4  model;
    mat4  prevModel;
    vec4  boundingSphere;
    uint  materialIndex;
    uint  shadeMode;
    uint  entityID;
    uint  boneOffset;
    uint  indexCount;
    uint  firstIndex;
    int   vertexOffset;
    uint  prevBoneOffset;
};

layout(std430, set = 5, binding = 0) readonly buffer ObjectBuffer {
    GPUObjectData objects[];
};

void main()
{
    GPUObjectData obj = objects[gl_BaseInstance];

    vec4 modelPos = vec4(a_Position, 1.0);
    vec4 worldPos = obj.model * modelPos;

    v_TexCoord0 = a_TexCoord0;
    v_TexCoord1 = a_TexCoord1;

    // TBN identical to pbr.vert (Gram-Schmidt re-orthogonalization). Uses current
    // model only — TBN is consumed by the frag's normal-map sample which produces
    // current-frame world normal (motion vectors don't need a TBN derivative).
    mat3 normalMatrix = mat3(transpose(inverse(obj.model)));
    vec3 N = normalize(normalMatrix * a_Normal);
    vec3 T = normalize(mat3(obj.model) * a_Tangent);
    T = normalize(T - dot(T, N) * N);
    vec3 B = cross(N, T);
    v_TBN = mat3(T, B, N);

    v_MaterialIndex = obj.materialIndex;

    // Current vs previous clip — same vertex position in model space, different transforms.
    // Frag computes NDC delta in screen space as the motion vector.
    v_CurrClip  = ubo.viewProjection     * worldPos;
    v_PrevClip  = ubo.prevViewProjection * obj.prevModel * modelPos;
    gl_Position = v_CurrClip;
}
