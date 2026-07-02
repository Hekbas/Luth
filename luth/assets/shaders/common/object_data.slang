// Per-object data (Set 5 binding 0) shared by every raster vertex shader. Twin of the C++ GPUObjectData
// (DrawListBuilder): std430, 192 B/entry. The two deformed-buffer device addresses are typed as raw
// float* (BDA): a skinned/deformable VS reads the post-deform interleaved Vertex (13 floats/vert) straight
// from the seam buffer the RT BLAS + geometry table also consume, so raster==RT. A null pointer means the
// deformed buffer isn't ready yet (skinned BLAS still loading); the VS clips off-screen. see arch/rendering-pipeline.md
public struct GPUObjectData
{
    public float4x4 model;
    public float4x4 prevModel;
    public float4   boundingSphere;
    public uint     materialIndex;
    public uint     shadeMode;
    public uint     entityID;
    public uint     boneOffset;
    public uint     indexCount;
    public uint     firstIndex;
    public int      vertexOffset;
    public uint     prevBoneOffset;
    public float*   deformedBufCurr;   // was uvec2 deformedBdaCurr; BDA to this frame's deformed Vertex buffer
    public float*   deformedBufPrev;   // prev frame's deformed positions (motion vectors)
}
[[vk::binding(0, 5)]] public StructuredBuffer<GPUObjectData> objects;

// Slang has no inverse() (HLSL lineage); cofactor 3x3, mirrors common/material.slang.
public float3x3 Inverse3x3(float3x3 m)
{
    float3 a = float3(m[0][0], m[1][0], m[2][0]);
    float3 b = float3(m[0][1], m[1][1], m[2][1]);
    float3 c = float3(m[0][2], m[1][2], m[2][2]);
    float3 r0 = cross(b, c);
    float3 r1 = cross(c, a);
    float3 r2 = cross(a, b);
    float invDet = 1.0 / dot(a, r0);
    return float3x3(r0 * invDet, r1 * invDet, r2 * invDet);
}

// Inverse-transpose of the upper 3x3; preserves normal orientation under non-uniform scale.
public float3x3 NormalMatrix(float4x4 model)
{
    return transpose(Inverse3x3((float3x3)model));
}
