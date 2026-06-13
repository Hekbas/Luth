// Volumetric fog inject — scatter pass. Per froxel: CSM/RT-shadowed dir light + clustered point lights +
// IBL multi-scatter, HG phase-weighted + density-aware sun absorption, written as vec4(inScatter, 0).
// scat = albedo × J; integrate supplies the σ_t path factor — do NOT pre-multiply by density here.
// see arch/rendering-pipeline.md
import globals;
import restir_common;       // DirectionalLightData / PointLightData
import material;            // GtGeomEntry + GT_VIS_SOLID
import material_bindings_rt; // RtConfirmAlphaCandidates (Set 3 material, Set 4 bindless)

[[vk::binding(1, 0)]] SamplerCube irradianceMap;
[[vk::binding(6, 0)]] RaytracingAccelerationStructure topLevelAS;
[[vk::binding(0, 1)]] Sampler3D volDensity;       // .r density, .gba tint
[[vk::binding(1, 1)]] [[vk::image_format("rgba16f")]] RWTexture3D<float4> volInScatter;
[[vk::binding(2, 1)]] ByteAddressBuffer       g_Lights;
[[vk::binding(3, 1)]] StructuredBuffer<uint2> g_ClusterGrid;
[[vk::binding(4, 1)]] StructuredBuffer<uint>  g_LightIndex;
[[vk::binding(5, 1)]] Sampler2DArrayShadow    shadowMap;

struct PC
{
    float4x4     invView;
    uint         volDimX, volDimY, volDimZ, _pad;
    GtGeomEntry* geomTable;
};
[[vk::push_constant]] PC pc;

static const uint  k_ClusterTilesX  = 16u;
static const uint  k_ClusterTilesY  =  9u;
static const uint  k_ClusterSlicesZ = 48u;
static const float PI = 3.14159265359;

DirectionalLightData LoadDirLight()
{
    DirectionalLightData d;
    d.direction = asfloat(g_Lights.Load3(0));
    d.intensity = asfloat(g_Lights.Load(12));
    d.color     = asfloat(g_Lights.Load3(16));
    d._pad      = 0.0;
    return d;
}
PointLightData LoadPoint(uint j)
{
    uint b = 48u + j * 32u;
    PointLightData p;
    p.position  = asfloat(g_Lights.Load3(b + 0u));
    p.range     = asfloat(g_Lights.Load(b + 12u));
    p.color     = asfloat(g_Lights.Load3(b + 16u));
    p.intensity = asfloat(g_Lights.Load(b + 28u));
    return p;
}

float HenyeyGreenstein(float cosTheta, float g)
{
    float g2 = g * g;
    float denom = 1.0 + g2 - 2.0 * g * cosTheta;
    return (1.0 - g2) / (4.0 * PI * pow(max(denom, 1e-4), 1.5));
}

float SliceToViewZ(int i, float nearZ, float farZ)
{
    return nearZ * pow(farZ / nearZ, (float(i) + 0.5) / float(pc.volDimZ));
}

uint ComputeClusterIDForVoxel(int2 voxelXY, float viewZ)
{
    float sliceF = log(max(viewZ, ubo.nearZ) / ubo.nearZ) / log(ubo.farZ / ubo.nearZ) * float(k_ClusterSlicesZ);
    uint  slice = uint(clamp(floor(sliceF), 0.0, float(k_ClusterSlicesZ - 1u)));
    float2 tileScale = float2(float(pc.volDimX), float(pc.volDimY)) / float2(float(k_ClusterTilesX), float(k_ClusterTilesY));
    uint2 tile = uint2(floor(float2(voxelXY) / tileScale));
    tile.x = min(tile.x, k_ClusterTilesX - 1u);
    tile.y = min(tile.y, k_ClusterTilesY - 1u);
    return slice * k_ClusterTilesX * k_ClusterTilesY + tile.y * k_ClusterTilesX + tile.x;
}

uint SelectCascade(float viewDistance)
{
    for (uint i = 0u; i < 4u; ++i)
        if (viewDistance < ubo.cascadeSplitsViewZ[i]) return i;
    return 3u;
}

float SampleCSM(float3 worldPos, uint cascade)
{
    if (ubo.shadowBias[cascade] < 0.0) return 1.0;
    float4 sc = mul(ubo.lightSpaceMatrix[cascade], float4(worldPos, 1.0));
    float3 ndc = sc.xyz / sc.w;
    if (ndc.x < -1.0 || ndc.x > 1.0 || ndc.y < -1.0 || ndc.y > 1.0 || ndc.z < 0.0 || ndc.z > 1.0)
        return 1.0;
    float2 uv = ndc.xy * 0.5 + 0.5;
    float depthRef = ndc.z - ubo.shadowBias[cascade];
    return shadowMap.SampleCmp(float3(uv, float(cascade)), depthRef);
}

// World→atlas UVW for sun-ray density sampling; .w = 0 when outside the camera frustum atlas.
float4 WorldToAtlasUVW(float3 worldPos)
{
    float3 vp = mul(ubo.view, float4(worldPos, 1.0)).xyz;
    float  vz = -vp.z;
    if (vz < ubo.nearZ || vz > ubo.farZ) return float4(0.0);
    float2 uv = float2(vp.x * ubo.projection[0][0] / -vp.z, vp.y * ubo.projection[1][1] / -vp.z) * 0.5 + 0.5;
    if (any(uv < 0.0) || any(uv > 1.0)) return float4(0.0);
    float slice = log(vz / ubo.nearZ) / log(ubo.farZ / ubo.nearZ);
    return float4(uv, slice, 1.0);
}

// Sun-fog absorption: N-step density-atlas march toward the sun (out-of-frustum samples contribute zero).
float SunFogTransmittance(float3 worldPos, int steps)
{
    if (steps <= 0) return 1.0;
    float3 toSun = -LoadDirLight().direction;
    const float stepLen = 2.0;
    float opticalDepth = 0.0;
    for (int s = 1; s <= steps; ++s)
    {
        float4 uvw = WorldToAtlasUVW(worldPos + toSun * (float(s) * stepLen));
        if (uvw.w < 0.5) continue;
        opticalDepth += volDensity.SampleLevel(uvw.xyz, 0.0).r * stepLen;
    }
    return exp(-opticalDepth);
}

// RT shadow ray (froxel is free space → no normal offset). Drops the opaque flag so cutout instances
// surface as candidates, alpha-tested in RtConfirmAlphaCandidates (god rays carve leaf holes).
bool Visible(float3 x, float3 dir, float maxDist)
{
    float  sMin  = ubo.rtShadowParams.y;
    float3 sOrig = x + dir * sMin;
    RayDesc ray; ray.Origin = sOrig; ray.TMin = sMin; ray.Direction = dir; ray.TMax = max(maxDist, sMin);
    RayQuery<RAY_FLAG_NONE> sq;
    sq.TraceRayInline(topLevelAS, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH, GT_VIS_SOLID, ray);
    RtConfirmAlphaCandidates(sq, pc.geomTable);
    return sq.CommittedStatus() == COMMITTED_NOTHING;
}

[shader("compute")]
[numthreads(8, 8, 4)]
void main(uint3 tid : SV_DispatchThreadID)
{
    int3 voxel  = int3(tid);
    int3 volDim = int3(int(pc.volDimX), int(pc.volDimY), int(pc.volDimZ));
    if (any(voxel >= volDim)) return;

    // Voxel center → world (matches inject_density).
    float2 uv    = (float2(voxel.xy) + 0.5) / float2(volDim.xy);
    float2 ndcXY = uv * 2.0 - 1.0;
    float  viewZ = SliceToViewZ(voxel.z, ubo.nearZ, ubo.farZ);
    float  vsZ   = -viewZ;
    float  vsX   = ndcXY.x * (-vsZ) / ubo.projection[0][0];
    float  vsY   = ndcXY.y * (-vsZ) / ubo.projection[1][1];
    float3 worldPos     = mul(pc.invView, float4(float3(vsX, vsY, vsZ), 1.0)).xyz;
    float3 viewDirWorld = normalize(worldPos - ubo.cameraPos);

    float4 d4 = volDensity.Load(int4(voxel, 0));
    float  density = d4.r;
    float3 tint    = d4.gba;
    if (density < 1e-4) { volInScatter[uint3(voxel)] = float4(0.0); return; }

    float gAniso   = ubo.volTemporalParams.x;
    int   sunSteps = int(ubo.volTemporalParams.z);
    bool  rtFog    = ubo.volScatterParams.z > 0.5;

    DirectionalLightData dl = LoadDirLight();

    // Dir light: CSM/RT shadow × HG forward-scatter × density-aware sun absorption.
    float3 dirContrib = float3(0.0);
    if (dl.intensity > 0.0)
    {
        float shadow = rtFog ? (Visible(worldPos, -dl.direction, 1.0e30) ? 1.0 : 0.0)
                             : SampleCSM(worldPos, SelectCascade(viewZ));
        float phase  = HenyeyGreenstein(dot(dl.direction, -viewDirWorld), gAniso);
        float sunAbs = SunFogTransmittance(worldPos, sunSteps);
        dirContrib = dl.color * dl.intensity * shadow * phase * sunAbs;
    }

    // Clustered point lights — windowed 1/d² × HG phase.
    float3 pointContrib = float3(0.0);
    {
        uint  cid = ComputeClusterIDForVoxel(voxel.xy, viewZ);
        uint2 lc  = g_ClusterGrid[cid];
        for (uint j = 0u; j < lc.y; ++j)
        {
            PointLightData p = LoadPoint(g_LightIndex[lc.x + j]);
            float3 toLight = p.position - worldPos;
            float  dist2 = dot(toLight, toLight);
            if (dist2 > p.range * p.range) continue;
            float  dist     = sqrt(dist2);
            float  window   = 1.0 - clamp(dist / p.range, 0.0, 1.0); window *= window;
            float  atten    = (1.0 / max(dist2, 0.01)) * window;
            float3 incident = -toLight / max(dist, 1e-4);
            float  phase    = HenyeyGreenstein(dot(incident, -viewDirWorld), gAniso);
            if (rtFog && !Visible(worldPos, -incident, dist - 2.0 * ubo.rtShadowParams.y)) continue;
            pointContrib += p.color * p.intensity * atten * phase;
        }
    }

    float3 inScatter = tint * (dirContrib + pointContrib);

    // 2nd-order multi-scatter: IBL ambient × albedo (survives in shadow).
    float msK = ubo.heightFogParams.w;
    if (msK > 0.0)
        inScatter += irradianceMap.SampleLevel(float3(0.0, 1.0, 0.0), 0.0).rgb * ubo.iblIntensity * msK * tint;

    // Artistic post-canonical multiplier (UE5/Frostbite knob — energy non-conserving by design).
    inScatter *= ubo.volScatterParams.x;

    volInScatter[uint3(voxel)] = float4(inScatter, 0.0);
}
