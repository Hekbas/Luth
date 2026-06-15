#version 460
#extension GL_EXT_buffer_reference        : require
#extension GL_EXT_buffer_reference_uvec2  : require

// Slim G-buffer vertex shader (skinned). Reads the deformation seam: CURRENT clip from the CURR
// deformed region (+ TBN), PREVIOUS clip from the PREV region — the double-buffered deformed buffer
// replaces the dual-region bone skin for motion vectors. see arch/multi-queue.md

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

// Set 5: Per-object data SSBO (std430, 192 bytes per entry)
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
    uvec2 deformedBdaCurr;
    uvec2 deformedBdaPrev;
};

layout(std430, set = 5, binding = 0) readonly buffer ObjectBuffer {
    GPUObjectData objects[];
};

// Deformation seam — post-skin object-space vertex (interleaved Vertex, 13 floats/vert). CURR holds
// this frame's skin (pos/normal/uv/tangent); PREV holds last frame's positions for motion vectors.
layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer DeformedBuf {
    float verts[];
};

void main()
{
    GPUObjectData obj = objects[gl_BaseInstance];

    // Guard: deformed buffer not ready (skinned BLAS still loading) — clip off-screen, no null deref.
    if (obj.deformedBdaCurr == uvec2(0u)) { gl_Position = vec4(0.0, 0.0, 2.0, 1.0); return; }

    DeformedBuf dbCurr = DeformedBuf(obj.deformedBdaCurr);
    DeformedBuf dbPrev = DeformedBuf(obj.deformedBdaPrev);
    uint b = uint(gl_VertexIndex) * 13u;
    vec3 dPos     = vec3(dbCurr.verts[b +  0u], dbCurr.verts[b +  1u], dbCurr.verts[b +  2u]);
    vec3 dNrm     = vec3(dbCurr.verts[b +  3u], dbCurr.verts[b +  4u], dbCurr.verts[b +  5u]);
    vec2 dUV0     = vec2(dbCurr.verts[b +  6u], dbCurr.verts[b +  7u]);
    vec2 dUV1     = vec2(dbCurr.verts[b +  8u], dbCurr.verts[b +  9u]);
    vec3 dTan     = vec3(dbCurr.verts[b + 10u], dbCurr.verts[b + 11u], dbCurr.verts[b + 12u]);
    vec3 dPosPrev = vec3(dbPrev.verts[b +  0u], dbPrev.verts[b +  1u], dbPrev.verts[b +  2u]);

    v_TexCoord0 = dUV0;
    v_TexCoord1 = dUV1;

    // TBN against the current deformed normal/tangent (matches the RT hit basis + pbr_skinned).
    mat3 normalMatrix = mat3(transpose(inverse(obj.model)));
    vec3 N = normalize(normalMatrix * dNrm);
    vec3 T = normalize(mat3(obj.model) * dTan);
    T = normalize(T - dot(T, N) * N);
    vec3 B = cross(N, T);
    v_TBN = mat3(T, B, N);

    v_MaterialIndex = obj.materialIndex;

    v_CurrClip  = ubo.viewProjection     * obj.model     * vec4(dPos,     1.0);
    v_PrevClip  = ubo.prevViewProjection * obj.prevModel * vec4(dPosPrev, 1.0);
    gl_Position = v_CurrClip;
}
