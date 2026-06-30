// Forward+ light-to-cluster assignment. One invocation per cluster: point + spot lights sphere-vs-AABB
// test against the cluster's view-space AABB; InterlockedAdd reserves a contiguous LightIndex range; the
// grid gets (offset, count). Spot indices carry SPOT_INDEX_FLAG. Standalone Set 0. The light SSBO is a
// header + flexible points[] (read via ByteAddressBuffer); spots reuse the points[] view (their first 16 B
// alias PointLightData). see arch/rendering-pipeline.md
static const uint TILES_X = 16u, TILES_Y = 9u, SLICES_Z = 48u;
static const uint NUM_CLUSTERS = TILES_X * TILES_Y * SLICES_Z;
static const uint MAX_LIGHTS_PER_CLUSTER = 128u;
static const uint SPOT_INDEX_FLAG = 0x80000000u;

struct ClusterAABB { float4 minView; float4 maxView; };

[[vk::binding(0, 0)]] ByteAddressBuffer g_Lights;                       // header (48 B) + points[] (32 B each) @48
[[vk::binding(1, 0)]] StructuredBuffer<ClusterAABB> clusters;
[[vk::binding(2, 0)]] RWStructuredBuffer<uint2> grid;
[[vk::binding(3, 0)]] RWStructuredBuffer<uint> indices;
[[vk::binding(4, 0)]] RWStructuredBuffer<uint> counter;

struct PC
{
    float4x4 view;
    uint     pointLightCount;
    uint     spotLightCount;
    uint     maxLightsPerCluster;
    uint     _pad;
};
[[vk::push_constant]] PC pc;

// points[] entry j: position@(48+j*32), range@(+12). Spots alias the same layout (pos+range first 16 B).
float3 LoadPos(uint j)   { return asfloat(g_Lights.Load3(48u + j * 32u)); }
float  LoadRange(uint j) { return asfloat(g_Lights.Load(48u + j * 32u + 12u)); }

[shader("compute")]
[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    uint clusterID = tid.x;
    if (clusterID >= NUM_CLUSTERS) return;

    float3 cMin = clusters[clusterID].minView.xyz;
    float3 cMax = clusters[clusterID].maxView.xyz;

    uint local[MAX_LIGHTS_PER_CLUSTER];
    uint localCount = 0u;

    for (uint i = 0u; i < pc.pointLightCount; ++i)
    {
        float3 posVS = mul(pc.view, float4(LoadPos(i), 1.0)).xyz;
        float  r = LoadRange(i);
        float3 closest = clamp(posVS, cMin, cMax);
        float3 d = posVS - closest;
        if (dot(d, d) <= r * r && localCount < pc.maxLightsPerCluster)
        {
            local[localCount] = i;
            localCount += 1u;
        }
    }

    // Spots: SpotLightData is 64 B = 2 PointLightData slots; its first 16 B alias pos+range.
    for (uint j = 0u; j < pc.spotLightCount; ++j)
    {
        uint slot = pc.pointLightCount + 2u * j;
        float3 posVS = mul(pc.view, float4(LoadPos(slot), 1.0)).xyz;
        float  r = LoadRange(slot);
        float3 closest = clamp(posVS, cMin, cMax);
        float3 d = posVS - closest;
        if (dot(d, d) <= r * r && localCount < pc.maxLightsPerCluster)
        {
            local[localCount] = SPOT_INDEX_FLAG | j;
            localCount += 1u;
        }
    }

    uint base = 0u;
    if (localCount > 0u) InterlockedAdd(counter[0], localCount, base);
    grid[clusterID] = uint2(base, localCount);
    for (uint k = 0u; k < localCount; ++k)
        indices[base + k] = local[k];
}
