// Shared forward lighting for PBR consumers — Set 3 cluster-light declarations, CSM cascade
// sampling, the clustered point-light loop, and split-sum IBL ambient. Pairs with
// common/pbr_surface.glsl; included by pbr.frag and the transparent-pass shaders.
//
// CONTRACT: includer includes common/globals.glsl first (ubo.*). Set 3 here is bindings 0-3 only —
// the opaque-depth-coupled screen-space inputs (sun mask b4, ReSTIR DI b5 / GI b6, reflections b7)
// stay declared by pbr.frag; transparent consumers must never sample them (wrong at glass depth).

#ifndef LUTH_SHADERS_COMMON_PBR_LIGHTING
#define LUTH_SHADERS_COMMON_PBR_LIGHTING

#ifndef PBR_LIGHT_SET
#define PBR_LIGHT_SET 3
#endif

// Set 0: IBL textures (b1-b3; b0 = GlobalUniforms via globals.glsl)
layout(set = 0, binding = 1) uniform samplerCube irradianceMap;
layout(set = 0, binding = 2) uniform samplerCube prefilteredMap;
layout(set = 0, binding = 3) uniform sampler2D   brdfLUT;

// Forward+ lighting. b0 = LightSSBO (header + flexible point-light array, std430), b1 =
// ClusterGrid (uvec2 offset+count per cluster), b2 = LightIndex (flat indices into points[]),
// b3 = shadow sampler. Cluster ID layout matches cluster_build.comp / light_assign.comp.
struct DirectionalLightData {
    vec3  direction;
    float intensity;
    vec3  color;
    float _pad;
};

struct PointLightData {
    vec3  position;
    float range;
    vec3  color;
    float intensity;
};

const uint k_ClusterTilesX  = 16u;
const uint k_ClusterTilesY  =  9u;
const uint k_ClusterSlicesZ = 48u;

layout(std430, set = PBR_LIGHT_SET, binding = 0) readonly buffer LightBuffer {
    DirectionalLightData dirLight;
    uint                 pointLightCount;
    uint                 _pad[3];
    PointLightData       points[];
} lights;

layout(std430, set = PBR_LIGHT_SET, binding = 1) readonly buffer ClusterBuffer {
    uvec2 clusters[];   // (offset, count) into LightIndex
} clusterGrid;

layout(std430, set = PBR_LIGHT_SET, binding = 2) readonly buffer LightIndexBuffer {
    uint indices[];
} lightIndex;

layout(set = PBR_LIGHT_SET, binding = 3) uniform sampler2DArrayShadow shadowMap;

uint ComputeClusterID(vec4 fragCoord, vec2 viewportPx, float nearZ, float farZ) {
    // Linearize the perspective depth in fragCoord.z; Olsson logarithmic slice index.
    float linDepth = (nearZ * farZ) / (farZ - fragCoord.z * (farZ - nearZ));
    uint slice = uint(floor(log(max(linDepth, nearZ) / nearZ) / log(farZ / nearZ) * float(k_ClusterSlicesZ)));
    slice = clamp(slice, 0u, k_ClusterSlicesZ - 1u);

    uvec2 tile = uvec2(fragCoord.xy / (viewportPx / vec2(k_ClusterTilesX, k_ClusterTilesY)));
    tile.x = min(tile.x, k_ClusterTilesX - 1u);
    tile.y = min(tile.y, k_ClusterTilesY - 1u);

    return slice * k_ClusterTilesX * k_ClusterTilesY + tile.y * k_ClusterTilesX + tile.x;
}

const float GI_PI = 3.14159265358979;

// Cook-Torrance BRDF (D_GGX/G_Smith/F_Schlick/EvalBRDFTimesNdotL) + GGX-VNDF sampling: the single
// shared seam, byte-identical in raster and RT. Requires GI_PI (above) per the brdf.glsl contract.
#include "common/brdf.glsl"

// Fresnel-Schlick with roughness (for IBL — accounts for rough surfaces reducing reflections)
vec3 FresnelSchlickRoughness(float cosTheta, vec3 F0, float roughness)
{
    return F0 + (max(vec3(1.0 - roughness), F0) - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

// ---------- PCF Shadow ----------

// Project a world position into cascade `c`. Returns projected xy (remapped to [0,1]) and depth,
// plus an `inside` flag that's true when the ENTIRE 3×3 PCF footprint fits inside the cascade's
// ortho box. The shadow sampler is CLAMP_TO_BORDER + OPAQUE_WHITE — any PCF tap that spills past
// the edge returns 1.0 (lit), which produces a bright seam right before a cascade boundary. By
// insetting the inside-test by 2×texelSize (covers the 3×3 neighborhood we sample), we force
// fall-through into the next (larger) cascade early enough that every tap lands on real data.
// `depthBias` is reserved on the near Z-boundary so that after SamplePCF subtracts it, the
// reference depth can't slip below 0 and cause border-clamp false positives (light leaks).
struct CascadeProj { vec3 proj; bool inside; };

CascadeProj ProjectInCascade(vec3 worldPos, int c, vec2 texelSize, float depthBias)
{
    vec4 lsPos = ubo.lightSpaceMatrix[c] * vec4(worldPos, 1.0);
    vec3 proj  = lsPos.xyz / lsPos.w;
    proj.xy    = proj.xy * 0.5 + 0.5;

    // XY inset matches the 3×3 PCF footprint (2 texels on each axis, round up).
    vec2  xyLo    = vec2(2.0 * texelSize);
    vec2  xyHi    = vec2(1.0) - xyLo;
    bool  insideXY = all(greaterThanEqual(proj.xy, xyLo)) &&
                     all(lessThanEqual   (proj.xy, xyHi));
    // Reserve depthBias worth of headroom at the near plane. proj.z >= 1.0 is still valid —
    // hardware compare (LESS) will mark it lit, which matches "no caster in front" semantics.
    bool  insideZ  = (proj.z >= max(depthBias, 0.0)) && (proj.z <= 1.0);
    return CascadeProj(proj, insideXY && insideZ);
}

// 3x3 PCF using a pre-computed projection (see ProjectInCascade).
// Depth bias is slope-scaled: surfaces at grazing angle to the light need a larger
// offset to avoid self-shadowing (acne). The bias is multiplied by clamp(tan(θ), 1, 10)
// where θ is the angle between the surface normal and the light direction.
float SamplePCF(vec3 proj, int cascade, float depthBias, float NdotL, vec2 texelSize)
{
    float slopeFactor = clamp(tan(acos(NdotL)), 1.0, 10.0);
    proj.z -= depthBias * slopeFactor;
    float shadow = 0.0;
    for (int x = -1; x <= 1; ++x)
    {
        for (int y = -1; y <= 1; ++y)
        {
            vec4 sampleCoord = vec4(proj.xy + vec2(x, y) * texelSize, float(cascade), proj.z);
            shadow += texture(shadowMap, sampleCoord);
        }
    }
    return shadow / 9.0;
}

// Cascade selection:
//  1) viewZ picks the slice-appropriate cascade (by construction the fragment is spatially inside).
//  2) If that cascade's PCF footprint escapes the ortho box (edge taps would read the border),
//     step up to a larger cascade until one fits or we exhaust all four.
//  3) Blend into cascade+1 over the last `cascadeBlendWidth` of the slice's viewZ range.
// Normal bias nudges the sample along N to mitigate acne on grazing surfaces.
// `cascade` receives the chosen cascade index (0–3) or -1 for debug visualization.
struct ShadowResult { float shadow; int cascade; };

ShadowResult ComputeShadowCSM(vec3 worldPos, vec3 N)
{
    // Negative bias[0] = shadows globally disabled (CastShadows=false sentinel).
    if (ubo.shadowBias.x < 0.0)
        return ShadowResult(1.0, -1);

    float NdotL     = max(dot(N, normalize(-lights.dirLight.direction)), 0.0);
    vec2  texelSize = 1.0 / vec2(textureSize(shadowMap, 0).xy);
    float viewZ     = abs((ubo.view * vec4(worldPos, 1.0)).z);

    // viewZ → slice cascade. cascadeSplitsViewZ holds FAR viewZ per cascade.
    int primary = 3;
    for (int i = 0; i < 3; ++i)
    {
        if (viewZ <= ubo.cascadeSplitsViewZ[i]) { primary = i; break; }
    }

    // Upgrade to larger cascade if the (normal-biased) projection escapes primary's ortho box.
    // Normal bias is expressed in TEXELS and scaled by each cascade's world-space texel size so
    // the offset produces equivalent pixel-space nudges across cascades of very different extents
    // (cascade 3 can be ~8× cascade 0 yet share the same shadow resolution).
    int  chosen = -1;
    vec3 projChosen;
    for (int i = primary; i < 4; ++i)
    {
        float nBias  = ubo.shadowNormalBias[i] * ubo.cascadeTexelSize[i] * (1.0 - NdotL);
        vec3  biased = worldPos + N * nBias;
        CascadeProj p = ProjectInCascade(biased, i, texelSize, ubo.shadowBias[i]);
        if (p.inside)
        {
            chosen     = i;
            projChosen = p.proj;
            break;
        }
    }

    // Fragment escapes every cascade from primary up. Sampler would border-clamp to lit anyway,
    // so return 1.0 and let the fragment be fully lit (rare — implies world extent > cascade 3).
    if (chosen < 0)
        return ShadowResult(1.0, -1);

    float sA = SamplePCF(projChosen, chosen, ubo.shadowBias[chosen], NdotL, texelSize);

    // Blend into next cascade near the far edge of THIS slice's viewZ range (only when the
    // fragment was served by its primary cascade — fall-through cases are already oversized).
    float blendStart = 1.0 - ubo.cascadeBlendWidth;
    if (chosen == primary && chosen < 3)
    {
        float nearSplit = (chosen == 0) ? 0.0 : ubo.cascadeSplitsViewZ[chosen - 1];
        float farSplit  = ubo.cascadeSplitsViewZ[chosen];
        float t         = (viewZ - nearSplit) / max(farSplit - nearSplit, 1e-4);
        float blend     = smoothstep(blendStart, 1.0, t);

        if (blend > 0.0)
        {
            int   next      = chosen + 1;
            float nBiasNext = ubo.shadowNormalBias[next] * ubo.cascadeTexelSize[next] * (1.0 - NdotL);
            vec3  nextPos   = worldPos + N * nBiasNext;
            CascadeProj pN  = ProjectInCascade(nextPos, next, texelSize, ubo.shadowBias[next]);
            if (pN.inside)
            {
                float sB = SamplePCF(pN.proj, next, ubo.shadowBias[next], NdotL, texelSize);
                return ShadowResult(mix(sA, sB, blend), chosen);
            }
        }
    }

    return ShadowResult(sA, chosen);
}

// Unshadowed Forward+ cluster loop — the point-light path for surfaces that can't consume the
// screen-space ReSTIR DI image (transparents) or when ReSTIR is disabled.
vec3 EvalClusteredPointLights(vec4 fragCoord, vec3 worldPos, vec3 V, vec3 N, vec3 albedo, float metallic, float roughness)
{
    vec3  Lo        = vec3(0.0);
    uint  clusterID = ComputeClusterID(fragCoord, ubo.viewportSize, ubo.nearZ, ubo.farZ);
    uvec2 oc        = clusterGrid.clusters[clusterID];
    uint  baseIdx   = oc.x;
    uint  lightCnt  = oc.y;
    for (uint k = 0u; k < lightCnt; ++k)
    {
        PointLightData pl = lights.points[lightIndex.indices[baseIdx + k]];
        vec3  toLight   = pl.position - worldPos;
        float dist      = length(toLight);
        float atten     = 1.0 / max(dist * dist, 0.0001);
        float rolloff   = pow(1.0 - clamp(dist / pl.range, 0.0, 1.0), 2.0);
        vec3  ptRadiance = pl.color * pl.intensity * atten * rolloff;
        if (dot(ptRadiance, ptRadiance) > 0.0001)
            Lo += EvalBRDFTimesNdotL(normalize(toLight), ptRadiance, V, N, albedo, metallic, roughness);
    }
    return Lo;
}

// Split-sum IBL ambient. envBRDF + specularIBL are returned alongside the ambient term because
// pbr.frag's RT-reflection composite swaps specularIBL out for the traced radiance above the
// roughness cutoff (reflRad·envBRDF − specularIBL·iblIntensity).
struct IblResult { vec3 ambient; vec3 envBRDF; vec3 specularIBL; };

IblResult EvalIblAmbient(vec3 N, vec3 V, vec3 albedo, float metallic, float roughness, float ao, float iblIntensity)
{
    vec3 F0 = mix(vec3(0.04), albedo, metallic);
    vec3 F  = FresnelSchlickRoughness(max(dot(N, V), 0.0), F0, roughness);
    vec3 kD = (1.0 - F) * (1.0 - metallic);

    vec3 irradiance = texture(irradianceMap, N).rgb;
    vec3 diffuseIBL = irradiance * albedo;

    const float MAX_REFLECTION_LOD = 4.0;
    vec3 R = reflect(-V, N);
    vec3 prefilteredColor = textureLod(prefilteredMap, R, roughness * MAX_REFLECTION_LOD).rgb;
    vec2 brdf = texture(brdfLUT, vec2(max(dot(N, V), 0.0), roughness)).rg;

    IblResult r;
    r.envBRDF     = F * brdf.x + brdf.y;
    r.specularIBL = prefilteredColor * r.envBRDF;
    r.ambient     = (kD * diffuseIBL + r.specularIBL) * ao * iblIntensity;
    return r;
}

#endif
