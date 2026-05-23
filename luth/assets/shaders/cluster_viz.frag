#version 450

// Forward+ cluster density viz. True 3D — samples SceneDepth at the fragment, linearizes it,
// derives the Olsson logarithmic slice index, and reads the per-cluster light count from the
// ClusterGrid (set 1 binding 1 = same Set 3 layout the PBR pipeline uses). Tint heat-maps the
// count; sky / far-plane pixels stay transparent so the underlying scene shows through.

const uint k_ClusterTilesX  = 16u;
const uint k_ClusterTilesY  =  9u;
const uint k_ClusterSlicesZ = 24u;
const uint k_MaxLightsPerCluster = 128u;

layout(location = 0) in  vec2 v_TexCoord;
layout(location = 0) out vec4 outColor;

// Set 0: scene depth sampler (per-view, written once at AllocateViewResources time).
layout(set = 0, binding = 0) uniform sampler2D u_SceneDepth;

// Set 1: per-view lightDescSet (cycled per frame). Only b1 (ClusterGrid) is declared here.
layout(std430, set = 1, binding = 1) readonly buffer ClusterBuffer {
    uvec2 clusters[];
} clusterGrid;

layout(push_constant) uniform PC {
    vec2  viewportSize;
    float nearZ;
    float farZ;
} pc;

vec3 HeatColor(float t)
{
    t = clamp(t, 0.0, 1.0);
    if (t < 0.33)      return mix(vec3(0.0, 0.2, 0.7), vec3(0.0, 0.7, 0.2), smoothstep(0.0, 0.33, t));
    else if (t < 0.66) return mix(vec3(0.0, 0.7, 0.2), vec3(0.95, 0.85, 0.1), smoothstep(0.33, 0.66, t));
    else               return mix(vec3(0.95, 0.85, 0.1), vec3(0.95, 0.15, 0.05), smoothstep(0.66, 1.0, t));
}

void main()
{
    float depth = texture(u_SceneDepth, v_TexCoord).r;

    // Sky / far-plane fragments — skip cluster lookup; the depth=1.0 slice would otherwise show
    // up as a hot blob across the entire skybox region.
    if (depth >= 0.9999)
    {
        outColor = vec4(0.0);
        return;
    }

    float linDepth = (pc.nearZ * pc.farZ) / (pc.farZ - depth * (pc.farZ - pc.nearZ));
    uint slice = uint(floor(log(max(linDepth, pc.nearZ) / pc.nearZ) / log(pc.farZ / pc.nearZ) * float(k_ClusterSlicesZ)));
    slice = clamp(slice, 0u, k_ClusterSlicesZ - 1u);

    uvec2 tile = uvec2(v_TexCoord * vec2(k_ClusterTilesX, k_ClusterTilesY));
    tile.x = min(tile.x, k_ClusterTilesX - 1u);
    tile.y = min(tile.y, k_ClusterTilesY - 1u);

    uint clusterID = slice * k_ClusterTilesX * k_ClusterTilesY + tile.y * k_ClusterTilesX + tile.x;
    uint count = clusterGrid.clusters[clusterID].y;

    float t = float(count) / float(k_MaxLightsPerCluster);
    outColor = vec4(HeatColor(t), 0.6);
}
