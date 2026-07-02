// Forward+ cluster AABB builder. One invocation per cluster: unprojects the four NDC tile corners to
// view-space rays, computes Olsson logarithmic view-space Z bounds, intersects rays with z-planes for the
// 8 view-space corners, writes the AABB, and zero-clears the ClusterGrid. Standalone Set 0.
// see arch/rendering-pipeline.md
static const uint SLICES_Z = 48u;

struct ClusterAABB
{
    float4 minView;   // .xyz view-space min; .w padding for std430 alignment
    float4 maxView;
};
[[vk::binding(0, 0)]] RWStructuredBuffer<ClusterAABB> clusters;
[[vk::binding(1, 0)]] RWStructuredBuffer<uint2> grid;   // (offset, count); produced by LightAssign, zero-clear here

struct PC
{
    float4x4 invProjection;
    float2   viewportSize;
    float2   _pad;
    float    nearZ;
    float    farZ;
    uint     tilesX;
    uint     tilesY;
};
[[vk::push_constant]] PC pc;

float3 ScreenToView(float3 ndc)
{
    float4 v = mul(pc.invProjection, float4(ndc, 1.0));
    return v.xyz / v.w;
}
float3 IntersectZPlane(float3 dir, float zPlane) { return dir * (zPlane / dir.z); }

[shader("compute")]
[numthreads(4, 4, 4)]
void main(uint3 tid : SV_DispatchThreadID)
{
    uint x = tid.x, y = tid.y, z = tid.z;
    if (x >= pc.tilesX || y >= pc.tilesY || z >= SLICES_Z) return;

    uint clusterID = z * pc.tilesX * pc.tilesY + y * pc.tilesX + x;

    // NDC tile corners (min/max of all 8 corners -> AABB, so Y orientation is irrelevant).
    float2 ndcMin = float2(float(x)     / float(pc.tilesX) * 2.0 - 1.0, float(y)     / float(pc.tilesY) * 2.0 - 1.0);
    float2 ndcMax = float2(float(x + 1) / float(pc.tilesX) * 2.0 - 1.0, float(y + 1) / float(pc.tilesY) * 2.0 - 1.0);

    float3 vTL = ScreenToView(float3(ndcMin.x, ndcMin.y, 0.0));
    float3 vTR = ScreenToView(float3(ndcMax.x, ndcMin.y, 0.0));
    float3 vBL = ScreenToView(float3(ndcMin.x, ndcMax.y, 0.0));
    float3 vBR = ScreenToView(float3(ndcMax.x, ndcMax.y, 0.0));

    // Olsson logarithmic depth slicing: view-space z is negative in a right-handed view frame.
    float zNear = -pc.nearZ * pow(pc.farZ / pc.nearZ, float(z)     / float(SLICES_Z));
    float zFar  = -pc.nearZ * pow(pc.farZ / pc.nearZ, float(z + 1) / float(SLICES_Z));

    float3 c0 = IntersectZPlane(vTL, zNear);
    float3 c1 = IntersectZPlane(vTR, zNear);
    float3 c2 = IntersectZPlane(vBL, zNear);
    float3 c3 = IntersectZPlane(vBR, zNear);
    float3 c4 = IntersectZPlane(vTL, zFar);
    float3 c5 = IntersectZPlane(vTR, zFar);
    float3 c6 = IntersectZPlane(vBL, zFar);
    float3 c7 = IntersectZPlane(vBR, zFar);

    float3 mn = min(min(min(c0, c1), min(c2, c3)), min(min(c4, c5), min(c6, c7)));
    float3 mx = max(max(max(c0, c1), max(c2, c3)), max(max(c4, c5), max(c6, c7)));

    clusters[clusterID].minView = float4(mn, 0.0);
    clusters[clusterID].maxView = float4(mx, 0.0);
    grid[clusterID] = uint2(0u, 0u);
}
