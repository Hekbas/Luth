// Per-vertex compute skinning for the RT BLAS refit + RT geometry table (and the raster vertex source).
// Reads the source SkinnedVertex VB via a raw BDA float* (scalar/tight 84 B = 21 floats); reads bone
// matrices from the engine-shared BoneMatrixBuffer SSBO at set 0 binding 0; writes a deformed vertex in
// the interleaved Vertex layout (13 floats) to an output BDA the BLAS refit + geometry table consume.
//
// CONTRACT: input layout MUST match Model.h::SkinnedVertex (scalar/tight, 84 B / 21 floats): pos@0,
// normal@3, uv0@6, uv1@8, tangent@10, boneIDs@13 (int, negative = "no bone"), boneWeights@17 — locked by
// static_assert in VulkanAccelerationStructure.cpp.
//
// CONTRACT: the deformed output MUST match the Vertex layout material.slang::GatherHitGeometry reads —
// 13 floats/vert: pos@0-2, normal@3-5, uv0@6-7, uv1@8-9, tangent@10-12. Normal/tangent use mat3(skin),
// mirroring pbr_skinned so raster==RT once raster reads this buffer.
//
// CONTRACT: Bones SSBO is BoneMatrixBuffer's current-frame region; pc.boneOffset is the entity's base
// matrix index inside it, populated by AnimationSystem and surfaced through MeshDrawSnapshot.

[[vk::binding(0, 0)]] StructuredBuffer<float4x4> bones;

struct PC
{
    float* inputBuf;     // raw float view of SkinnedVertex[] (21 floats/vert), scalar/tight
    float* deformedBuf;  // interleaved Vertex out (13 floats/vert)
    uint   vertexCount;
    uint   boneOffset;
};
[[vk::push_constant]] PC pc;

[shader("compute")]
[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    const uint vid = tid.x;
    if (vid >= pc.vertexCount) return;

    const uint inB = vid * 21u;   // 21 floats per SkinnedVertex (84 B)
    float3 position    = float3(pc.inputBuf[inB +  0u], pc.inputBuf[inB +  1u], pc.inputBuf[inB +  2u]);
    float3 normal      = float3(pc.inputBuf[inB +  3u], pc.inputBuf[inB +  4u], pc.inputBuf[inB +  5u]);
    float2 uv0         = float2(pc.inputBuf[inB +  6u], pc.inputBuf[inB +  7u]);
    float2 uv1         = float2(pc.inputBuf[inB +  8u], pc.inputBuf[inB +  9u]);
    float3 tangent     = float3(pc.inputBuf[inB + 10u], pc.inputBuf[inB + 11u], pc.inputBuf[inB + 12u]);
    int4   boneIDs     = int4(asint(pc.inputBuf[inB + 13u]), asint(pc.inputBuf[inB + 14u]),
                              asint(pc.inputBuf[inB + 15u]), asint(pc.inputBuf[inB + 16u]));
    float4 boneWeights = float4(pc.inputBuf[inB + 17u], pc.inputBuf[inB + 18u],
                                pc.inputBuf[inB + 19u], pc.inputBuf[inB + 20u]);

    // Blended skin matrix (LBS) — identical construction to pbr_skinned so the deformed vertex matches
    // the raster vertex bit-for-bit once raster reads this buffer.
    float4x4 skin = (float4x4)0.0;
    for (int i = 0; i < 4; ++i)
    {
        const int boneId = boneIDs[i];
        if (boneId < 0) continue;
        skin += boneWeights[i] * bones[pc.boneOffset + uint(boneId)];
    }
    // No-bones sentinel: a zero diagonal means an unweighted vertex → rigid passthrough.
    if (skin[0][0] == 0.0 && skin[1][1] == 0.0 && skin[2][2] == 0.0)
        skin = float4x4(1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1);

    const float3   outPos = mul(skin, float4(position, 1.0)).xyz;
    const float3x3 skin3  = (float3x3)skin;
    const float3   outNrm = normalize(mul(skin3, normal));
    const float3   outTan = normalize(mul(skin3, tangent));

    const uint base = vid * 13u;            // 13 floats per interleaved Vertex
    pc.deformedBuf[base +  0u] = outPos.x;
    pc.deformedBuf[base +  1u] = outPos.y;
    pc.deformedBuf[base +  2u] = outPos.z;
    pc.deformedBuf[base +  3u] = outNrm.x;
    pc.deformedBuf[base +  4u] = outNrm.y;
    pc.deformedBuf[base +  5u] = outNrm.z;
    pc.deformedBuf[base +  6u] = uv0.x;
    pc.deformedBuf[base +  7u] = uv0.y;
    pc.deformedBuf[base +  8u] = uv1.x;
    pc.deformedBuf[base +  9u] = uv1.y;
    pc.deformedBuf[base + 10u] = outTan.x;
    pc.deformedBuf[base + 11u] = outTan.y;
    pc.deformedBuf[base + 12u] = outTan.z;
}
