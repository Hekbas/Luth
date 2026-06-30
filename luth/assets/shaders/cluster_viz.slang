// Forward+ cluster density viz. Samples SceneDepth, linearizes, derives the Olsson log slice, reads the
// per-cluster light count from the ClusterGrid (Set 1 b1 = the PBR Set 3 layout), heat-maps the count.
// see arch/rendering-pipeline.md
static const uint k_ClusterTilesX = 16u, k_ClusterTilesY = 9u, k_ClusterSlicesZ = 48u;
static const uint k_MaxLightsPerCluster = 128u;

[[vk::binding(0, 0)]] Sampler2D u_SceneDepth;
[[vk::binding(1, 1)]] StructuredBuffer<uint2> clusterGrid;   // (offset, count) per cluster

struct PC
{
    float2 viewportSize;
    float  nearZ;
    float  farZ;
};
[[vk::push_constant]] PC pc;

float3 HeatColor(float t)
{
    t = clamp(t, 0.0, 1.0);
    if (t < 0.33)      return lerp(float3(0.0, 0.2, 0.7), float3(0.0, 0.7, 0.2), smoothstep(0.0, 0.33, t));
    else if (t < 0.66) return lerp(float3(0.0, 0.7, 0.2), float3(0.95, 0.85, 0.1), smoothstep(0.33, 0.66, t));
    else               return lerp(float3(0.95, 0.85, 0.1), float3(0.95, 0.15, 0.05), smoothstep(0.66, 1.0, t));
}

[shader("fragment")]
float4 main([[vk::location(0)]] float2 v_TexCoord) : SV_Target
{
    float depth = u_SceneDepth.Sample(v_TexCoord).r;
    if (depth >= 0.9999) return float4(0.0);   // sky / far plane → transparent

    float linDepth = (pc.nearZ * pc.farZ) / (pc.farZ - depth * (pc.farZ - pc.nearZ));
    uint slice = uint(floor(log(max(linDepth, pc.nearZ) / pc.nearZ) / log(pc.farZ / pc.nearZ) * float(k_ClusterSlicesZ)));
    slice = clamp(slice, 0u, k_ClusterSlicesZ - 1u);

    uint2 tile = uint2(v_TexCoord * float2(k_ClusterTilesX, k_ClusterTilesY));
    tile.x = min(tile.x, k_ClusterTilesX - 1u);
    tile.y = min(tile.y, k_ClusterTilesY - 1u);

    uint clusterID = slice * k_ClusterTilesX * k_ClusterTilesY + tile.y * k_ClusterTilesX + tile.x;
    uint count = clusterGrid[clusterID].y;

    float t = float(count) / float(k_MaxLightsPerCluster);
    return float4(HeatColor(t), 0.6);
}
