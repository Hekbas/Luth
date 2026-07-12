// Shared forward lighting for PBR raster consumers: CSM cascade sampling, the clustered point-light loop,
// and split-sum IBL ambient. Twin of pbr_lighting.glsl (deleted once pbr + transparent convert).
// Set 3 here is bindings 0-3 only; the opaque-depth-coupled screen-space inputs stay with the consumer.
// see arch/rendering-pipeline.md
import globals;        // ubo
import restir_common;  // DirectionalLightData / PointLightData / SpotLightData (shared std430 layout) + Luminance
import brdf;           // EvalBRDFTimesNdotL

// Set 0 b1-b3: IBL (b0 = GlobalUniforms via globals).
[[vk::binding(1, 0)]] SamplerCube u_IrradianceMap;
[[vk::binding(2, 0)]] SamplerCube u_PrefilteredMap;
[[vk::binding(3, 0)]] Sampler2D   u_BrdfLUT;

// Set 3 b0-b3: light SSBO (byte-loaded header+array) + cluster grid + light-index list + CSM shadow array.
[[vk::binding(0, 3)]] ByteAddressBuffer       g_Lights;
[[vk::binding(1, 3)]] StructuredBuffer<uint2> g_ClusterGrid;   // (offset, count) into the index list
[[vk::binding(2, 3)]] StructuredBuffer<uint>  g_LightIndex;
[[vk::binding(3, 3)]] Sampler2DArrayShadow    u_ShadowMap;

public static const float GI_PI = 3.14159265358979;   // remodulation constant shared with the consumer

public static const uint k_ClusterTilesX  = 16u;
public static const uint k_ClusterTilesY  =  9u;
public static const uint k_ClusterSlicesZ = 48u;

// Cluster light-index tag: spot indices carry the high bit (set by light_assign.comp); plain indices are points.
public static const uint SPOT_INDEX_FLAG = 0x80000000u;

// LightBuffer header = DirectionalLightData(32) + count(4) + pad[3](12); points[] (32 B each) at offset 48.
public DirectionalLightData LoadDirLight()
{
    DirectionalLightData d;
    d.direction = asfloat(g_Lights.Load3(0));
    d.intensity = asfloat(g_Lights.Load(12));
    d.color     = asfloat(g_Lights.Load3(16));
    d._pad      = 0.0;
    return d;
}
public PointLightData LoadPoint(uint j)
{
    uint b = 48u + j * 32u;
    PointLightData p;
    p.position  = asfloat(g_Lights.Load3(b + 0u));
    p.range     = asfloat(g_Lights.Load(b + 12u));
    p.color     = asfloat(g_Lights.Load3(b + 16u));
    p.intensity = asfloat(g_Lights.Load(b + 28u));
    return p;
}

// spots[] follow points[] in the same SSBO: base = 48 + pointCount*32 + j*64 (64 B stride).
public SpotLightData LoadSpot(uint j)
{
    uint pointCount = g_Lights.Load(32u);
    uint b = 48u + pointCount * 32u + j * 64u;
    SpotLightData s;
    s.position  = asfloat(g_Lights.Load3(b + 0u));
    s.range     = asfloat(g_Lights.Load(b + 12u));
    s.direction = asfloat(g_Lights.Load3(b + 16u));
    s.cosInner  = asfloat(g_Lights.Load(b + 28u));
    s.color     = asfloat(g_Lights.Load3(b + 32u));
    s.intensity = asfloat(g_Lights.Load(b + 44u));
    s.cosOuter  = asfloat(g_Lights.Load(b + 48u));
    s._pad0 = s._pad1 = s._pad2 = 0.0;
    return s;
}

public uint ComputeClusterID(float4 fragCoord, float2 viewportPx, float nearZ, float farZ)
{
    float linDepth = (nearZ * farZ) / (farZ - fragCoord.z * (farZ - nearZ));   // Olsson log slice
    uint  slice = uint(floor(log(max(linDepth, nearZ) / nearZ) / log(farZ / nearZ) * float(k_ClusterSlicesZ)));
    slice = clamp(slice, 0u, k_ClusterSlicesZ - 1u);
    uint2 tile = uint2(fragCoord.xy / (viewportPx / float2(k_ClusterTilesX, k_ClusterTilesY)));
    tile.x = min(tile.x, k_ClusterTilesX - 1u);
    tile.y = min(tile.y, k_ClusterTilesY - 1u);
    return slice * k_ClusterTilesX * k_ClusterTilesY + tile.y * k_ClusterTilesX + tile.x;
}

// Fresnel-Schlick with roughness (IBL: rough surfaces reduce reflections).
public float3 FresnelSchlickRoughness(float cosTheta, float3 F0, float roughness)
{
    return F0 + (max(float3(1.0 - roughness), F0) - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

// ---- CSM PCF shadow ----
public struct CascadeProj { public float3 proj; public bool inside; }

CascadeProj ProjectInCascade(float3 worldPos, int c, float2 texelSize, float depthBias)
{
    float4 lsPos = mul(ubo.lightSpaceMatrix[c], float4(worldPos, 1.0));
    float3 proj  = lsPos.xyz / lsPos.w;
    proj.xy      = proj.xy * 0.5 + 0.5;
    float2 xyLo  = float2(2.0 * texelSize);   // inset matches the 3x3 PCF footprint
    float2 xyHi  = float2(1.0) - xyLo;
    bool insideXY = all(proj.xy >= xyLo) && all(proj.xy <= xyHi);
    bool insideZ  = (proj.z >= max(depthBias, 0.0)) && (proj.z <= 1.0);
    CascadeProj r; r.proj = proj; r.inside = insideXY && insideZ;
    return r;
}

// 3x3 PCF; slope-scaled depth bias. shadowMap is a comparison sampler -> SampleCmp returns the PCF result.
float SamplePCF(float3 proj, int cascade, float depthBias, float NdotL, float2 texelSize)
{
    float slopeFactor = clamp(tan(acos(NdotL)), 1.0, 10.0);
    proj.z -= depthBias * slopeFactor;
    float shadow = 0.0;
    for (int x = -1; x <= 1; ++x)
        for (int y = -1; y <= 1; ++y)
            shadow += u_ShadowMap.SampleCmp(float3(proj.xy + float2(x, y) * texelSize, float(cascade)), proj.z);
    return shadow / 9.0;
}

public struct ShadowResult { public float shadow; public int cascade; }

public ShadowResult ComputeShadowCSM(float3 worldPos, float3 N)
{
    ShadowResult res;
    if (ubo.shadowBias.x < 0.0) { res.shadow = 1.0; res.cascade = -1; return res; }   // shadows disabled

    DirectionalLightData dl = LoadDirLight();
    float NdotL = max(dot(N, normalize(-dl.direction)), 0.0);
    uint sw, sh, selems; u_ShadowMap.GetDimensions(sw, sh, selems);
    float2 texelSize = 1.0 / float2(sw, sh);
    float viewZ = abs(mul(ubo.view, float4(worldPos, 1.0)).z);

    int primary = 3;
    for (int i = 0; i < 3; ++i)
        if (viewZ <= ubo.cascadeSplitsViewZ[i]) { primary = i; break; }

    int    chosen = -1;
    float3 projChosen = float3(0.0);
    for (int i = primary; i < 4; ++i)
    {
        float  nBias  = ubo.shadowNormalBias[i] * ubo.cascadeTexelSize[i] * (1.0 - NdotL);
        CascadeProj p = ProjectInCascade(worldPos + N * nBias, i, texelSize, ubo.shadowBias[i]);
        if (p.inside) { chosen = i; projChosen = p.proj; break; }
    }
    if (chosen < 0) { res.shadow = 1.0; res.cascade = -1; return res; }

    float sA = SamplePCF(projChosen, chosen, ubo.shadowBias[chosen], NdotL, texelSize);

    // Blend into the next cascade near the far edge of this slice's viewZ range.
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
            CascadeProj pN  = ProjectInCascade(worldPos + N * nBiasNext, next, texelSize, ubo.shadowBias[next]);
            if (pN.inside)
            {
                float sB = SamplePCF(pN.proj, next, ubo.shadowBias[next], NdotL, texelSize);
                res.shadow = lerp(sA, sB, blend); res.cascade = chosen; return res;
            }
        }
    }
    res.shadow = sA; res.cascade = chosen;
    return res;
}

// Unshadowed Forward+ cluster loop: point + spot lights. Spot indices carry SPOT_INDEX_FLAG; the cone
// term gates the shared point distance/range falloff. spotsOnly skips points when ReSTIR DI already
// supplied them via the demodulated image; spots aren't in the DI reservoir yet, so they ride here.
public float3 EvalClusteredLights(float4 fragCoord, float3 worldPos, float3 V, SurfaceBRDF s, bool spotsOnly)
{
    float3 Lo        = float3(0.0);
    uint   clusterID = ComputeClusterID(fragCoord, ubo.viewportSize, ubo.nearZ, ubo.farZ);
    uint2  oc        = g_ClusterGrid[clusterID];
    for (uint k = 0u; k < oc.y; ++k)
    {
        uint   idx    = g_LightIndex[oc.x + k];
        bool   isSpot = (idx & SPOT_INDEX_FLAG) != 0u;
        if (spotsOnly && !isSpot) continue;

        float3 position, color;
        float  range, intensity, cone = 1.0;
        if (isSpot)
        {
            SpotLightData sl = LoadSpot(idx & 0x7FFFFFFFu);
            position = sl.position; range = sl.range; color = sl.color; intensity = sl.intensity;
            float3 L = normalize(sl.position - worldPos);
            cone = smoothstep(sl.cosOuter, sl.cosInner, dot(-L, sl.direction));   // soft cone edge
        }
        else
        {
            PointLightData pl = LoadPoint(idx);
            position = pl.position; range = pl.range; color = pl.color; intensity = pl.intensity;
        }

        float3 toLight = position - worldPos;
        float  dist    = length(toLight);
        float  atten   = 1.0 / max(dist * dist, 0.0001);
        float  rolloff = pow(1.0 - clamp(dist / range, 0.0, 1.0), 2.0);
        float3 radiance = color * intensity * atten * rolloff * cone;
        if (dot(radiance, radiance) > 0.0001)
            Lo += EvalSurfBRDFTimesNdotL(normalize(toLight), radiance, V, s);
    }
    return Lo;
}

// Split-sum IBL ambient. envBRDF + specularIBL returned so the consumer's RT-reflection composite can swap
// specularIBL out for traced radiance above the roughness cutoff.
public struct IblResult { public float3 ambient; public float3 envBRDF; public float3 specularIBL; }

public IblResult EvalIblAmbient(SurfaceBRDF s, float3 V, float ao, float iblIntensity)
{
    float  NoV = max(dot(s.N, V), 0.0);
    float3 F0  = lerp(float3(0.04), s.albedo, s.metallic);
    float3 F   = FresnelSchlickRoughness(NoV, F0, s.roughness);
    float3 kD  = (1.0 - F) * (1.0 - s.metallic);
    if (s.transmission > 0.0) kD *= (1.0 - s.transmission * (1.0 - s.metallic));  // glass: diffuse IBL -> transmission

    float3 diffuseIBL = u_IrradianceMap.Sample(s.N).rgb * s.albedo;

    const float MAX_REFLECTION_LOD = 4.0;
    // Anisotropic bent reflection (McAuley/Filament): warp N toward the stretched-highlight axis.
    float3 R;
    if (abs(s.anisotropy) > 1.0e-3)
    {
        float3 dir  = (s.anisotropy >= 0.0) ? s.B : s.T;
        float3 nA   = cross(cross(dir, V), dir);
        float  bend = abs(s.anisotropy) * saturate(5.0 * s.roughness);
        R = reflect(-V, normalize(lerp(s.N, nA, bend)));
    }
    else
        R = reflect(-V, s.N);
    float3 prefilteredColor = u_PrefilteredMap.SampleLevel(R, s.roughness * MAX_REFLECTION_LOD).rgb;
    float2 brdf = u_BrdfLUT.Sample(float2(NoV, s.roughness)).rg;

    IblResult r;
    r.envBRDF     = F * brdf.x + brdf.y;
    r.specularIBL = prefilteredColor * r.envBRDF;

    float3 base = kD * diffuseIBL + r.specularIBL;

    // Sheen env lobe UNDER the coat: split-sum with E_Sheen as the (Fresnel-free) BRDF term; the GGX
    // prefilter at the sheen roughness stands in for a Charlie prefilter (Filament does the same). Folding
    // sheenScale into envBRDF/specularIBL keeps the consumer's RT-reflection swap correct -- the fuzz
    // attenuates the base spec whether it comes from the cube or the tracer.
    float sheenW = max(s.sheenColor.r, max(s.sheenColor.g, s.sheenColor.b));
    if (sheenW > 0.0)
    {
        float  sr = clamp(s.sheenRoughness, 0.04, 1.0);
        float  Es = E_Sheen(NoV, sr);
        float3 sheenEnv = u_PrefilteredMap.SampleLevel(reflect(-V, s.N), sr * MAX_REFLECTION_LOD).rgb
                        * (s.sheenColor * Es);
        float  sheenScale = 1.0 - sheenW * Es;
        base = base * sheenScale + sheenEnv;
        r.envBRDF     *= sheenScale;
        r.specularIBL *= sheenScale;
    }

    if (s.clearcoat > 0.0)
    {
        // Isotropic F0=0.04 coat env lobe on top; base attenuates by (1-Fc). Folding (1-Fc) into
        // envBRDF/specularIBL keeps the consumer's RT-reflection composite (base-spec swap) correct.
        float  Fc  = s.clearcoat * (0.04 + 0.96 * pow(1.0 - NoV, 5.0));
        float  ccR = clamp(s.clearcoatRoughness, 0.04, 1.0);
        float3 Rc  = reflect(-V, s.N);
        float2 ccB = u_BrdfLUT.Sample(float2(NoV, ccR)).rg;
        float3 ccSpec = u_PrefilteredMap.SampleLevel(Rc, ccR * MAX_REFLECTION_LOD).rgb * (0.04 * ccB.x + ccB.y);
        base = base * (1.0 - Fc) + ccSpec;
        r.envBRDF     *= (1.0 - Fc);
        r.specularIBL *= (1.0 - Fc);
    }
    r.ambient = base * ao * iblIntensity;
    return r;
}
