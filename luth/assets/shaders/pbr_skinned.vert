#version 460
#extension GL_EXT_buffer_reference        : require
#extension GL_EXT_buffer_reference_uvec2  : require

layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec3 a_Normal;
layout(location = 2) in vec2 a_TexCoord0;
layout(location = 3) in vec2 a_TexCoord1;
layout(location = 4) in vec3 a_Tangent;
layout(location = 5) in ivec4 a_BoneIDs;
layout(location = 6) in vec4 a_BoneWeights;

layout(location = 0) out vec3 v_WorldPos;
layout(location = 1) out vec3 v_Normal;
layout(location = 2) out vec2 v_TexCoord0;
layout(location = 6) out vec2 v_TexCoord1;
layout(location = 3) out mat3 v_TBN;    // consumes locations 3, 4, 5
layout(location = 7) flat out uint v_MaterialIndex;
layout(location = 8) flat out uint v_ShadeMode;
layout(location = 9) flat out uint v_EntityID;

// Set 0: Global Uniforms
layout(set = 0, binding = 0) uniform GlobalUniforms {
    mat4 viewProjection;
    mat4 prevViewProjection;  // frame N-1's VP — motion vectors + TAA reprojection
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

// Set 4: Bone Matrices SSBO
layout(std430, set = 4, binding = 0) readonly buffer BoneMatrices {
    mat4 bones[];
};

// Set 5: Per-object data SSBO (std430, 176 bytes per entry)
struct GPUObjectData {
    mat4  model;          // 64B
    mat4  prevModel;      // 64B — frame N-1's worldMatrix
    vec4  boundingSphere; // 16B
    uint  materialIndex;  // 4B
    uint  shadeMode;      // 4B
    uint  entityID;       // 4B
    uint  boneOffset;     // 4B
    uint  indexCount;     // 4B
    uint  firstIndex;     // 4B
    int   vertexOffset;   // 4B
    uint  prevBoneOffset; // 4B — prev-frame bones region (dual-region BoneMatrixBuffer)
    uvec2 deformedBdaCurr;
    uvec2 deformedBdaPrev;
};

layout(std430, set = 5, binding = 0) readonly buffer ObjectBuffer {
    GPUObjectData objects[];
};

// Deformation seam — the post-skin object-space vertex (interleaved Vertex, 13 floats: pos, normal,
// uv0, uv1, tangent), the same buffer the RT BLAS + geometry table read. Fetched by gl_VertexIndex.
layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer DeformedBuf {
    float verts[];
};

void main()
{
    GPUObjectData obj = objects[gl_BaseInstance];

    // Guard: deformed buffer not ready (skinned BLAS still loading) — clip off-screen, no null deref.
    if (obj.deformedBdaCurr == uvec2(0u)) { gl_Position = vec4(0.0, 0.0, 2.0, 1.0); return; }

    // Read the deformation seam instead of skinning here — raster == RT by shared source.
    DeformedBuf db = DeformedBuf(obj.deformedBdaCurr);
    uint b = uint(gl_VertexIndex) * 13u;
    vec3 dPos = vec3(db.verts[b +  0u], db.verts[b +  1u], db.verts[b +  2u]);
    vec3 dNrm = vec3(db.verts[b +  3u], db.verts[b +  4u], db.verts[b +  5u]);
    vec2 dUV0 = vec2(db.verts[b +  6u], db.verts[b +  7u]);
    vec2 dUV1 = vec2(db.verts[b +  8u], db.verts[b +  9u]);
    vec3 dTan = vec3(db.verts[b + 10u], db.verts[b + 11u], db.verts[b + 12u]);

    vec4 worldPos = obj.model * vec4(dPos, 1.0);
    v_WorldPos = worldPos.xyz;
    v_TexCoord0 = dUV0;
    v_TexCoord1 = dUV1;

    // Inverse-transpose preserves orientation under non-uniform scale.
    mat3 normalMatrix = mat3(transpose(inverse(obj.model)));
    v_Normal = normalize(normalMatrix * dNrm);

    // TBN, Gram-Schmidt re-orthogonalized.
    vec3 T = normalize(mat3(obj.model) * dTan);
    vec3 N = v_Normal;
    T = normalize(T - dot(T, N) * N);
    vec3 B = cross(N, T);
    v_TBN = mat3(T, B, N);

    v_MaterialIndex = obj.materialIndex;
    v_ShadeMode     = obj.shadeMode;
    v_EntityID      = obj.entityID;

    gl_Position = ubo.viewProjection * worldPos;
}
