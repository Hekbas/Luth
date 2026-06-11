// Shared transparent-surface shading — included by pbr_transparent.frag (sorted blend) and
// pbr_oit_store.frag (PPLL store) so the two paths can't drift. Declares the transparent
// pipeline's pass-local resources (Set 0 b6 TLAS, Set 6 b0 fog atlas, the 16 B push-constant
// block) on top of the pbr_surface + pbr_lighting seams.
//
// CONTRACT: transparent fragments sit at depths the opaque prepass knows nothing about, so the
// opaque-coupled screen-space inputs are deliberately NOT consumed here: RT sun-shadow mask
// (replaced by a per-fragment rayQuery), ReSTIR DI/GI (replaced by the cluster loop + IBL),
// GTAO (material AO only). Tokuyoshi spec-AA is also omitted (opaque-path nicety). Fog is the
// froxel atlas sampled at the FRAGMENT's depth — the fullscreen composite already fogged the
// background to opaque depth; volumetric_composite.frag documents the shared blend algebra.
//
// Includer must enable: GL_EXT_nonuniform_qualifier, GL_EXT_ray_query, GL_EXT_buffer_reference2,
// GL_GOOGLE_include_directive, and include common/globals.glsl first.

#ifndef LUTH_SHADERS_COMMON_PBR_TRANSPARENT_SHADING
#define LUTH_SHADERS_COMMON_PBR_TRANSPARENT_SHADING

#include "common/pbr_surface.glsl"
#include "common/pbr_lighting.glsl"
#include "common/froxel.glsl"

// geom_table's own material/texture decls sit at set 3/4 — occupied by Light/Bones in the
// geometry pipeline layout. Alias them onto the pbr_surface seam's declarations instead
// (GtMaterial is field-for-field GPUMaterialData; both 80 B std430).
#define GT_NO_RESOURCE_DECLS
#define GtMaterial  GPUMaterialData
#define gtMaterials materials
#define gtTextures  globalTextures
#include "common/geom_table.glsl"

// Set 0 b6: TLAS. RtSubsystem keeps a persistent empty TLAS bound, so this static read is never
// null — rays against the empty TLAS simply miss (lit). Cutout instances are FORCE_NO_OPAQUE and
// alpha-tested by the candidate loop; transparent instances are excluded from the TLAS entirely.
layout(set = 0, binding = 6) uniform accelerationStructureEXT topLevelAS;

// Set 6 b0: resolved volumetric in-scatter atlas, parity-picked per frame (the volumetric
// composite's b1 rule). b1/b2 of Set 6 are the OIT heads/nodes — declared by pbr_oit_store.frag.
layout(set = 6, binding = 0) uniform sampler3D volInScatter;

layout(push_constant) uniform TransparentPC {
    GeomTable geomTable;     // cutout alpha-test on the shadow ray; 0 until the first real TLAS build
    uint      flags;         // bit0 = fog atlas valid (volumetric enabled this frame)
    uint      nodeCapacity;  // pbr_oit_store.frag only; unused by the sorted path
} pc;

const uint TRANSPARENT_FLAG_FOG = 1u;

// 1-spp alpha-tested sun-shadow ray — the rt_sun_shadows.comp recipe evaluated at the transparent
// fragment's own position (the screen-space R8 mask is opaque-depth-coupled, wrong for glass).
float TraceSunShadow(vec3 worldPos, vec3 N)
{
    // Negative bias[0] = shadows globally disabled (CastShadows=false sentinel).
    if (ubo.shadowBias.x < 0.0)
        return 1.0;

    vec3  L     = normalize(-lights.dirLight.direction);
    float NdotL = max(dot(N, L), 0.0);
    if (NdotL <= 0.0)
        return 0.0;

    // Wächter & Binder 2019 origin biasing (RT Gems Ch. 6) — same epsilons as rt_sun_shadows.comp.
    vec3 origin = worldPos
                + N * (ubo.rtShadowParams.z * max(1.0 - NdotL, 0.1))
                + L * ubo.rtShadowParams.y;

    rayQueryEXT rq;
    rayQueryInitializeEXT(rq, topLevelAS,
        gl_RayFlagsTerminateOnFirstHitEXT, GT_VIS_SOLID,
        origin, 0.0, L, 10000.0);
    RtConfirmAlphaCandidates(rq, pc.geomTable);
    return (rayQueryGetIntersectionTypeEXT(rq, true) == gl_RayQueryCommittedIntersectionNoneEXT) ? 1.0 : 0.0;
}

// Full transparent-surface radiance: sun (rayQuery / CSM by ShadowingMode) + clustered point
// lights + IBL ambient + emission, fogged at the fragment's depth. Returns straight alpha.
// Discards below the material's alphaCutoff (inside EvalPbrSurface).
vec4 EvalTransparentSurfaceColor(uint materialIndex, uint shadeMode, vec2 uv0, vec2 uv1,
                                 mat3 TBN, vec3 vertexNormal, bool frontFacing,
                                 vec3 worldPos, vec4 fragCoord)
{
    GPUMaterialData mat = materials[materialIndex];
    PbrSurface s = EvalPbrSurface(mat, uv0, uv1, TBN, vertexNormal, frontFacing);

    // ShadeMode debug overrides — parity with pbr.frag (alpha 1.0, matching the pre-split path).
    if (shadeMode == 1u)  return vec4(s.albedo.rgb, 1.0);       // Unlit
    if (shadeMode == 3u)  return vec4(s.N * 0.5 + 0.5, 1.0);    // Normals
    if (shadeMode == 13u) return vec4(s.emission, 1.0);         // Emission

    vec3 V  = normalize(ubo.cameraPos - worldPos);
    vec3 Lo = vec3(0.0);

    float sunVis = (ubo.rtShadowParams.x > 0.5)
                 ? TraceSunShadow(worldPos, s.N)
                 : ComputeShadowCSM(worldPos, s.N).shadow;
    vec3 dirRadiance = lights.dirLight.color * lights.dirLight.intensity;
    Lo += EvalBRDFTimesNdotL(normalize(-lights.dirLight.direction), dirRadiance, V, s.N,
                             s.albedo.rgb, s.metallic, s.roughness) * sunVis;

    Lo += EvalClusteredPointLights(fragCoord, worldPos, V, s.N, s.albedo.rgb, s.metallic, s.roughness);

    IblResult ibl = EvalIblAmbient(s.N, V, s.albedo.rgb, s.metallic, s.roughness, s.materialAO, ubo.iblIntensity);
    vec3 color = ibl.ambient + Lo + s.emission;

    // Per-fragment froxel fog: final = radiance * (1 - fogOpacity) + scatter, fogOpacity capped by
    // the distance-fog max like the composite (the /fogOpacity ×fogOpacity there cancels — scatter
    // always lands in full). Blue-noise slice dither omitted (composite-only nicety).
    if ((pc.flags & TRANSPARENT_FLAG_FOG) != 0u)
    {
        float viewZ = FroxelDepthToViewZ(fragCoord.z, ubo.nearZ, ubo.farZ);
        float slice = FroxelViewZToSlice(viewZ, ubo.nearZ, ubo.farZ);
        vec4  volS  = texture(volInScatter, vec3(fragCoord.xy / ubo.viewportSize, slice));
        float fogOpacity = clamp(1.0 - volS.a, 0.0, max(ubo.distanceFogParams.y, 0.001));
        color = color * (1.0 - fogOpacity) + volS.rgb;
    }

    return vec4(color, s.albedo.a);
}

#endif
