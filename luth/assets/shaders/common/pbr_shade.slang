// Shared forward-PBR shading body: everything after the bounded-surface decode. pbr.slang (stock decode)
// and the generated pbr_graph_<uuid>.slang (node-graph decode) both call PbrShadeSurface so a per-material
// graph swaps only the EvalGraph<F> call, never this 100-line shading path. see arch/rendering-pipeline.md
import globals;
import material;
import material_bindings_raster;   // RasterFetch (Set 1 bindless, Set 2 material SSBO)
import pbr_lighting;               // ComputeShadowCSM / EvalClusteredLights / EvalIblAmbient + GI_PI
import brdf;                       // EvalBRDFTimesNdotL
import restir_common;              // DirectionalLightData

// Set 0 b4/b5: GTAO buffer + settings (b1-b3 IBL via pbr_lighting; b0 globals).
[[vk::binding(4, 0)]] Sampler2D gtaoTex;
struct GTAOUBO
{
    float intensity, radius, falloff, power;
    int   sliceCount, stepsPerSlice, enabled, visualize;
    float2 invResolution, invFullResolution;
};
[[vk::binding(5, 0)]] ConstantBuffer<GTAOUBO> gtao;

// Set 3 b4-b8: opaque-depth-coupled screen-space inputs (b0-b3 lights/cluster/shadow via pbr_lighting).
[[vk::binding(4, 3)]] Sampler2D sunShadowMask;   // RT sun-shadow visibility (R8)
[[vk::binding(5, 3)]] Sampler2D diIrradiance;    // demodulated DI diffuse irradiance
[[vk::binding(6, 3)]] Sampler2D giIrradiance;    // demodulated GI indirect-diffuse irradiance
[[vk::binding(7, 3)]] Sampler2D reflRadiance;    // demodulated RT reflection radiance
[[vk::binding(8, 3)]] Sampler2D diSpecular;      // demodulated ReSTIR-DI specular (#154)

// pbr.vert delivers v_TBN as a mat3 at locations 3-5; read its columns as separate T/B/N (layout-safe).
// Members are public so the per-material entry shaders (pbr.slang / generated pbr_graph_*) can read them.
public struct VIn
{
    [[vk::location(0)]] public float3 worldPos;
    [[vk::location(1)]] public float3 normal;
    [[vk::location(2)]] public float2 uv0;
    [[vk::location(3)]] public float3 T;
    [[vk::location(4)]] public float3 B;
    [[vk::location(5)]] public float3 N;      // TBN 3rd column (== normal)
    [[vk::location(6)]] public float2 uv1;
    [[vk::location(7)]] public nointerpolation uint materialIndex;
    [[vk::location(8)]] public nointerpolation uint shadeMode;
    [[vk::location(9)]] public nointerpolation uint entityID;
};
public struct FOut
{
    [[vk::location(0)]] public float4 color    : SV_Target0;
    [[vk::location(1)]] public uint   entityID : SV_Target1;
};

// RT sun-shadow visibility — samples the per-view R8 mask at screen UV (no cascade selection in RT mode).
ShadowResult ComputeShadowRT(float2 uv)
{
    ShadowResult r; r.cascade = -1;
    r.shadow = (ubo.shadowBias.x < 0.0) ? 1.0 : sunShadowMask.Sample(uv).r;
    return r;
}
// Dispatch: rtShadowParams.x > 0.5 → RT mask sample; else cascade PCF.
ShadowResult ComputeShadow(float3 worldPos, float3 N, float2 uv)
{
    if (ubo.rtShadowParams.x > 0.5) return ComputeShadowRT(uv);
    return ComputeShadowCSM(worldPos, N);
}

// Shade a decoded surface. The caller supplies MaterialInputs (stock or graph-decoded) + the raw material
// (alphaCutoff / normal-map indices / flags) + the fragment varyings; this owns cutout, normal-map perturb,
// specular AA, shade-mode debug, lighting (CSM/cluster/IBL + ReSTIR DI/GI/specular + RT reflections), AO.
public FOut PbrShadeSurface(MaterialInputs mi, GPUMaterialData m, VIn i, bool frontFacing, float4 fragCoord)
{
    FOut o;
    o.entityID = i.entityID;   // always written for picking
    RasterFetch rf;

    // Cutout discard (Cutout: alphaCutoff > 0; Opaque: 0).
    if (mi.baseColor.a < m.alphaCutoff) discard;

    // World shading normal: normal-mapped (two-sided flipped) or the interpolated vertex normal.
    float3 N = ((m.flags & FLAG_HAS_NORMAL) != 0u)
             ? ApplyNormalMap<RasterFetch>(m, i.uv0, i.uv1, i.T, i.B, i.N, rf)
             : normalize(i.normal);
    // A graph driving Output.normal overrides the map/vertex normal (tangent space; identity (0,0,1) = no-op).
    if (any(mi.normal != float3(0.0, 0.0, 1.0)))
        N = ApplyTangentNormal(mi.normal, i.T, i.B, i.N);
    if (!frontFacing) N = -N;

    float4 albedo    = mi.baseColor;
    float3 emission  = mi.emissive;
    float  metallic  = mi.metallic;
    float  roughness = mi.roughness;

    // Tokuyoshi 2019 geometric specular AA — screen-space normal curvature lifted into roughness.
    if (ubo.specAaParams.x != 0.0)
    {
        float3 dNdu = ddx(N); float3 dNdv = ddy(N);
        float  sigma2   = ubo.specAaParams.y * ubo.specAaParams.y;
        float  variance = sigma2 * (dot(dNdu, dNdu) + dot(dNdv, dNdv));
        roughness = clamp(sqrt(roughness * roughness + variance), 0.04, 1.0);
    }

    // Shade-mode overrides.
    if (i.shadeMode == 1u)  { o.color = float4(albedo.rgb, 1.0);      return o; }   // Unlit
    if (i.shadeMode == 2u)  { o.color = float4(0.45, 0.85, 1.0, 1.0); return o; }   // Wireframe (flat unlit lines, not lit)
    if (i.shadeMode == 3u)  { o.color = float4(N * 0.5 + 0.5, 1.0);   return o; }   // Normals
    if (i.shadeMode == 4u)
    {
        float id = float(i.entityID);
        o.color = float4(frac(id * 0.123), frac(id * 0.456), frac(id * 0.789), 1.0);
        return o;
    }
    if (i.shadeMode == 13u) { o.color = float4(emission, 1.0);        return o; }   // Emission
    if (i.shadeMode == 14u) { o.color = float4(metallic.xxx, 1.0);    return o; }   // Metallic
    if (i.shadeMode == 15u) { o.color = float4(mi.occlusion.xxx, 1.0);return o; }   // Occlusion (material AO)

    // Raw screen-space signals — sampled where the lit shading reads them, gated on the feature
    // (the picker greys these when off). Ambient Occlusion (17) reuses the gtao.visualize path below.
    float2 dbgUv = fragCoord.xy / ubo.viewportSize;
    // Raw signals always claim their mode: show the sampled buffer when the feature is on, else a
    // magenta "unavailable" flag so the view isn't a silent fall-through to the lit image.
    if (i.shadeMode == 18u) { o.color = ubo.restirParams.y > 0.5 ? float4(giIrradiance.Sample(dbgUv).rgb, 1.0) : float4(1.0, 0.0, 1.0, 1.0); return o; }  // GI Raw
    if (i.shadeMode == 19u) { o.color = ubo.restirParams.x > 0.5 ? float4(diIrradiance.Sample(dbgUv).rgb, 1.0) : float4(1.0, 0.0, 1.0, 1.0); return o; }  // DI Raw
    if (i.shadeMode == 20u) { o.color = ubo.reflParams.x  > 0.5 ? float4(reflRadiance.Sample(dbgUv).rgb, 1.0) : float4(1.0, 0.0, 1.0, 1.0); return o; }  // RT Reflection Raw

    // Material-baked AO × screen-space GTAO (half-res R8).
    float ao = mi.occlusion;
    if (gtao.enabled != 0)
        ao *= gtaoTex.Sample(fragCoord.xy * gtao.invFullResolution).r;

    float3 V = normalize(ubo.cameraPos - i.worldPos);
    float3 Lo = float3(0.0);
    float2 screenUv = fragCoord.xy / ubo.viewportSize;

    // Directional light + shadow (cascade PCF or RT mask).
    ShadowResult sr = ComputeShadow(i.worldPos, N, screenUv);
    {
        DirectionalLightData dl = LoadDirLight();
        float3 dirRadiance = dl.color * dl.intensity;
        Lo += EvalBRDFTimesNdotL(normalize(-dl.direction), dirRadiance, V, N, albedo.rgb, metallic, roughness) * sr.shadow;
    }

    // Punctual lights: demodulated ReSTIR DI image (points only), else the Forward+ cluster loop. Remodulate
    // by diffuse albedo / PI (metals carry no Lambertian diffuse). Spots aren't in the DI reservoir, so when
    // DI is on we still run the cluster loop for spots only; when off, the loop does points + spots.
    if (ubo.restirParams.x > 0.5)
    {
        Lo += diIrradiance.Sample(screenUv).rgb * (albedo.rgb * (1.0 - metallic) / GI_PI);
        Lo += EvalClusteredLights(fragCoord, i.worldPos, V, N, albedo.rgb, metallic, roughness, true);
    }
    else
        Lo += EvalClusteredLights(fragCoord, i.worldPos, V, N, albedo.rgb, metallic, roughness, false);

    // ReSTIR GI — demodulated indirect diffuse, added on top (independent of the DI gate).
    if (ubo.restirParams.y > 0.5)
        Lo += giIrradiance.Sample(screenUv).rgb * (albedo.rgb * (1.0 - metallic) / GI_PI);

    // ReSTIR-DI specular (#154) — demodulated F0-free lobe, remodulated by F0.
    if (ubo.restirParams.z > 0.0)
    {
        float3 F0 = lerp(float3(0.04), albedo.rgb, metallic);
        Lo += diSpecular.Sample(screenUv).rgb * F0 * ubo.restirParams.z;
    }

    IblResult ibl = EvalIblAmbient(N, V, albedo.rgb, metallic, roughness, ao, ubo.iblIntensity);
    float3 color = ibl.ambient + Lo;

    // RT specular reflection (D.1) — supersedes IBL specular above the roughness cutoff. reflRad is a
    // traced scene reflection (decoupled from iblIntensity); subtract the iblSpecular it replaces.
    if (ubo.reflParams.x > 0.5)
    {
        float  reflWeight = 1.0 - smoothstep(ubo.reflParams.y, ubo.reflParams.z, roughness);
        float3 reflRad    = reflRadiance.Sample(screenUv).rgb;
        color += (reflRad * ibl.envBRDF - ibl.specularIBL * ubo.iblIntensity) * ao * reflWeight;
    }

    // Debug: raw GTAO buffer (gtao.visualize toggle or the AmbientOcclusion shade-mode).
    if (i.shadeMode == 17u || (i.shadeMode == 0u && gtao.visualize != 0))
    {
        float g = gtaoTex.Sample(fragCoord.xy * gtao.invFullResolution).r;
        o.color = float4(g, g, g, 1.0);
        return o;
    }

    // Debug: cascade tint (CSM) / raw RT mask. Driven by the global toggle or the ShadowCascades shade-mode.
    if (i.shadeMode == 16u || (i.shadeMode == 0u && ubo.debugVisualizeCascades > 0.5))
    {
        if (ubo.rtShadowParams.x > 0.5)
        {
            float vis = sunShadowMask.Sample(screenUv).r;
            o.color = float4(vis, vis, vis, 1.0);
            return o;
        }
        else if (sr.cascade >= 0)
        {
            float3 cascadeColors[4] = { float3(1.0, 0.2, 0.2), float3(0.2, 1.0, 0.2),
                                        float3(0.2, 0.2, 1.0), float3(1.0, 1.0, 0.2) };
            color = lerp(color, cascadeColors[sr.cascade], 0.25);
        }
    }

    o.color = float4(color + emission, albedo.a);
    return o;
}
