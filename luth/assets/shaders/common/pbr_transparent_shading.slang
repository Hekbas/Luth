// Shared transparent-surface shading (sorted-blend + PPLL-store paths). Bounded material decode
// (RasterFetch) + sun (rayQuery / CSM) + cluster lights + IBL + per-fragment froxel fog. Twin of
// pbr_transparent_shading.glsl. Transparent fragments sit at depths the opaque prepass doesn't know, so
// the opaque-coupled screen-space inputs are deliberately NOT consumed here. see arch/rendering-pipeline.md
import globals;
import material;
import material_bindings_raster;  // GetMaterial + RasterFetch + RtConfirmAlphaCandidates (Set 1/2)
import mat_graph_registry;        // EvalGraphVariant: stock decode (variant 0) or the project's graph override
import pbr_lighting;              // ComputeShadowCSM / EvalClusteredLights / EvalIblAmbient / LoadDirLight + GI_PI
import brdf;                      // EvalBRDFTimesNdotL
import restir_common;            // DirectionalLightData
import froxel;

// Set 0 b6: TLAS (RtSubsystem keeps a persistent empty TLAS bound; this static read is never null).
// Set 6 b0: resolved volumetric in-scatter atlas (b1/b2 are the OIT heads/nodes; pbr_oit_store only).
[[vk::binding(6, 0)]] RaytracingAccelerationStructure topLevelAS;
[[vk::binding(0, 6)]] Sampler3D volInScatter;

public struct TransparentPC
{
    public GtGeomEntry* geomTable;    // cutout alpha-test on the shadow ray; 0 until the first real TLAS build
    public uint         flags;        // bit0 = fog atlas valid (volumetric enabled this frame)
    public uint         nodeCapacity; // pbr_oit_store only
};
[[vk::push_constant]] public TransparentPC pc;

public static const uint TRANSPARENT_FLAG_FOG = 1u;

// 1-spp alpha-tested sun-shadow ray at the transparent fragment's own position (the screen-space R8 mask
// is opaque-depth-coupled, wrong for glass). Same epsilons as rt_sun_shadows.
float TraceSunShadow(float3 worldPos, float3 N)
{
    if (ubo.shadowBias.x < 0.0) return 1.0;   // shadows globally disabled
    DirectionalLightData dl = LoadDirLight();
    float3 L     = normalize(-dl.direction);
    float  NdotL = max(dot(N, L), 0.0);
    if (NdotL <= 0.0) return 0.0;
    float3 origin = worldPos + N * (ubo.rtShadowParams.z * max(1.0 - NdotL, 0.1)) + L * ubo.rtShadowParams.y;
    RayDesc ray; ray.Origin = origin; ray.TMin = 0.0; ray.Direction = L; ray.TMax = 10000.0;
    RayQuery<RAY_FLAG_NONE> rq;
    rq.TraceRayInline(topLevelAS, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH, GT_VIS_SOLID, ray);
    RtConfirmAlphaCandidates(rq, pc.geomTable);
    return (rq.CommittedStatus() == COMMITTED_NOTHING) ? 1.0 : 0.0;
}

// Full transparent radiance: sun + clustered point lights + IBL + emission, fogged at the fragment's
// depth. Discards below alphaCutoff (inside the decode). Returns straight alpha.
public float4 EvalTransparentSurfaceColor(uint materialIndex, uint shadeMode, float2 uv0, float2 uv1,
                                          float3 T, float3 B, float3 N, bool frontFacing,
                                          float3 worldPos, float4 fragCoord, float4 vertexColor)
{
    // Decode through the shared variant registry so a node-graph material renders its routing here
    // exactly as the RT hit does (material_bindings_rt twin); variant 0 = stock channel decode.
    GPUMaterialData m = GetMaterial(materialIndex);
    RasterFetch rf;
    rf.paramBase = materialIndex * MAT_GRAPH_STRIDE;
    rf.worldPos  = worldPos;
    rf.viewDir   = normalize(ubo.cameraPos - worldPos);
    rf.time      = ubo.time;
    rf.ndotv     = saturate(dot(normalize(N), rf.viewDir));
    rf.worldNormal = normalize(N);
    rf.tangentViewDir = float3(dot(rf.viewDir, T), dot(rf.viewDir, B), dot(rf.viewDir, N));
    MaterialInputs mi = EvalGraphVariant<RasterFetch>(GraphVariant(m.flags), m, uv0, uv1, rf);
    mi.baseColor *= vertexColor;   // per-vertex color modulates albedo (matches the opaque path); white = no-op
    if (mi.baseColor.a < m.alphaCutoff) discard;

    float3 nrm = ((m.flags & FLAG_HAS_NORMAL) != 0u)
               ? ApplyNormalMap<RasterFetch>(m, uv0, uv1, T, B, N, rf)
               : normalize(N);
    if (any(mi.normal != float3(0.0, 0.0, 1.0)))
        nrm = ApplyTangentNormal(mi.normal, T, B, N);
    if (!frontFacing) nrm = -nrm;

    // ShadeMode debug overrides for the material-channel modes (alpha 1.0). Screen-space / opaque-coupled
    // modes (AO, cascades, raw GI/DI/reflection, entityID) do not apply to transparent fragments: the
    // opaque-coupled inputs are not bound here (see the file header).
    if (shadeMode == 1u)  return float4(mi.baseColor.rgb, 1.0);    // Unlit
    if (shadeMode == 2u)  return float4(0.45, 0.85, 1.0, 1.0);     // Wireframe (flat)
    if (shadeMode == 3u)  return float4(nrm * 0.5 + 0.5, 1.0);     // Normals
    if (shadeMode == 13u) return float4(mi.emissive, 1.0);         // Emission
    if (shadeMode == 14u) return float4(mi.metallic.xxx, 1.0);     // Metallic
    if (shadeMode == 15u) return float4(mi.occlusion.xxx, 1.0);    // Occlusion

    float3 V  = normalize(ubo.cameraPos - worldPos);
    float3 Lo = float3(0.0);

    // Shared BRDF surface (base + clear-coat + anisotropy); transparent skips the opaque spec-AA pass.
    SurfaceBRDF surf = MakeSurfaceBRDF(nrm, T, B, true, mi.baseColor.rgb, mi.metallic, mi.roughness,
                                       mi.clearcoat, mi.clearcoatRoughness, mi.anisotropy, mi.anisotropyRotation,
                                       mi.transmission, mi.ior, frontFacing);

    float sunVis = (ubo.rtShadowParams.x > 0.5) ? TraceSunShadow(worldPos, nrm)
                                                : ComputeShadowCSM(worldPos, nrm).shadow;
    DirectionalLightData dl = LoadDirLight();
    float3 dirRadiance = dl.color * dl.intensity;
    Lo += EvalSurfBRDFTimesNdotL(normalize(-dl.direction), dirRadiance, V, surf) * sunVis;

    Lo += EvalClusteredLights(fragCoord, worldPos, V, surf, false);

    IblResult ibl = EvalIblAmbient(surf, V, mi.occlusion, ubo.iblIntensity);
    float3 color = ibl.ambient + Lo + mi.emissive;

    // Per-fragment froxel fog at the fragment's depth (the fullscreen composite already fogged opaque).
    if ((pc.flags & TRANSPARENT_FLAG_FOG) != 0u)
    {
        float  viewZ = FroxelDepthToViewZ(fragCoord.z, ubo.nearZ, ubo.farZ);
        float  slice = FroxelViewZToSlice(viewZ, ubo.nearZ, ubo.farZ);
        float4 volS  = volInScatter.Sample(float3(fragCoord.xy / ubo.viewportSize, slice));
        float  fogOpacity = clamp(1.0 - volS.a, 0.0, max(ubo.distanceFogParams.y, 0.001));
        color = color * (1.0 - fogOpacity) + volS.rgb;
    }

    return float4(color, mi.baseColor.a);
}
