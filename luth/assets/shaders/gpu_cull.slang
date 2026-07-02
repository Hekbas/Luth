// GPU frustum cull. One invocation per object: world-space bounding sphere vs the 6 frustum planes,
// writing instanceCount (1/0) into the per-draw indirect command. Standalone Set 0.
struct GPUObjectData
{
    float4x4 model;
    float4x4 prevModel;      // motion vectors; not consumed by cull
    float4   boundingSphere; // xyz = center (local space), w = radius (local space)
    uint     materialIndex;
    uint     shadeMode;
    uint     entityID;
    uint     boneOffset;
    uint     indexCount;
    uint     firstIndex;
    int      vertexOffset;
    uint     prevBoneOffset;
    uint2    deformedBdaCurr;
    uint2    deformedBdaPrev;
};
[[vk::binding(0, 0)]] StructuredBuffer<GPUObjectData> objects;

struct IndirectCommand
{
    uint indexCount;
    uint instanceCount;
    uint firstIndex;
    int  vertexOffset;
    uint firstInstance;
};
[[vk::binding(1, 0)]] RWStructuredBuffer<IndirectCommand> commands;

struct PC
{
    float4 frustumPlanes[6];   // 96 B
    uint   objectCount;
    uint   destOffset;         // command index offset (per-view-cascade region)
};
[[vk::push_constant]] PC pc;

[shader("compute")]
[numthreads(256, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    uint idx = tid.x;
    if (idx >= pc.objectCount) return;

    GPUObjectData obj = objects[idx];

    float3 center = mul(obj.model, float4(obj.boundingSphere.xyz, 1.0)).xyz;
    // Max model-matrix column length scales radius (handles non-uniform scale). GLSL mat[i] is a column;
    // Slang m[i] is a row, so transpose the upper-left 3x3 to read columns as rows.
    float3x3 m3 = transpose((float3x3)obj.model);
    float maxScale = max(length(m3[0]), max(length(m3[1]), length(m3[2])));
    float radius   = obj.boundingSphere.w * maxScale;

    bool visible = true;
    for (int i = 0; i < 6; ++i)
    {
        if (dot(pc.frustumPlanes[i].xyz, center) + pc.frustumPlanes[i].w < -radius)
        {
            visible = false;
            break;
        }
    }

    commands[pc.destOffset + idx].instanceCount = visible ? 1u : 0u;
}
