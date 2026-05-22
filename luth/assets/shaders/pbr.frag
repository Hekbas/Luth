#version 450
#extension GL_EXT_nonuniform_qualifier : enable

layout(location = 0) in vec3 v_WorldPos;
layout(location = 1) in vec3 v_Normal;
layout(location = 2) in vec2 v_TexCoord0;
layout(location = 6) in vec2 v_TexCoord1;
layout(location = 3) in mat3 v_TBN;    // locations 3, 4, 5
layout(location = 7) flat in uint v_MaterialIndex;
layout(location = 8) flat in uint v_ShadeMode;
layout(location = 9) flat in uint v_EntityID;

layout(location = 0) out vec4 outColor;
layout(location = 1) out uint outEntityID;

// ---------- Descriptor Sets ----------

// Set 0: Global Uniforms
layout(set = 0, binding = 0) uniform GlobalUniforms {
    mat4 viewProjection;
    mat4 prevViewProjection;  // frame N-1's VP — motion vectors + TAA reprojection
    mat4 view;
    mat4 projection;
    vec3 cameraPos;
    float time;
    mat4 lightSpaceMatrix[4];        // Per-cascade light-space matrices (Phase 13)
    vec4 cascadeSplitsViewZ;         // Far view-space depth per cascade
    vec4 shadowBias;                 // Per-cascade depth bias (x<0 = shadows disabled)
    vec4 shadowNormalBias;           // Per-cascade normal bias (in shadow-map texels)
    vec4 cascadeTexelSize;           // World-space size of one shadow texel per cascade
    float iblIntensity;
    float skyboxIntensity;
    float debugVisualizeCascades;    // 0 = off, 1 = tint by cascade
    float cascadeBlendWidth;         // fraction of slice depth range used for cross-cascade blending
    vec2  viewportSize;       // pixels — cluster ID + screen-space recon
    float nearZ;
    float farZ;
} ubo;

// Set 0: IBL textures
layout(set = 0, binding = 1) uniform samplerCube irradianceMap;
layout(set = 0, binding = 2) uniform samplerCube prefilteredMap;
layout(set = 0, binding = 3) uniform sampler2D   brdfLUT;

// Set 0: GTAO final AO buffer (half-res R8) + settings (epic #58)
layout(set = 0, binding = 4) uniform sampler2D   gtaoTex;
layout(set = 0, binding = 5, std140) uniform GTAOUBO {
    float intensity;
    float radius;
    float falloff;
    float power;
    int   sliceCount;
    int   stepsPerSlice;
    int   enabled;
    int   visualize;
    vec2  invResolution;
    vec2  invFullResolution;
} gtao;

// Set 1: Bindless Textures (combined image-samplers) + canonical sampler array.
// Binding 1 is declared so future shader paths (per-map sampler selection) can index it;
// today's PBR sampling still goes through binding 0's per-texture baked samplers.
layout(set = 1, binding = 0) uniform sampler2D globalTextures[];
layout(set = 1, binding = 1) uniform sampler   bindlessSamplers[];

// Set 2: Material SSBO
// flags layout: bits 0-7 = HAS_* per map; bits 16-23 = per-map UV index (2 bits each).
struct GPUMaterialData {
    vec4  color;
    uint  diffuseIndex;
    uint  normalIndex;
    uint  metalRoughIndex;
    uint  occlusionIndex;
    uint  emissiveIndex;
    uint  alphaIndex;
    uint  specularIndex;
    uint  thicknessIndex;
    float metalness;
    float roughness;
    float alphaCutoff;
    uint  flags;
};

layout(std430, set = 2, binding = 0) readonly buffer MaterialBuffer {
    GPUMaterialData materials[];
};

// Set 3: Lights + Shadow
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

layout(set = 3, binding = 0) uniform LightUBO {
    DirectionalLightData dirLight;
    PointLightData       pointLights[64];
    int                  numPointLights;
} lights;

layout(set = 3, binding = 3) uniform sampler2DArrayShadow shadowMap;

// ---------- Flag Constants ----------

const uint FLAG_HAS_NORMAL     = (1u << 0);
const uint FLAG_HAS_METALROUGH = (1u << 1);
const uint FLAG_HAS_OCCLUSION  = (1u << 2);
const uint FLAG_HAS_DIFFUSE    = (1u << 3);
const uint FLAG_HAS_EMISSIVE   = (1u << 4);
const uint FLAG_HAS_ALPHA      = (1u << 5);
const uint FLAG_HAS_SPECULAR   = (1u << 6);
const uint FLAG_HAS_THICKNESS  = (1u << 7);

// UV index bit positions within flags (2 bits each, values 0-3)
const uint UV_SHIFT_DIFFUSE    = 16u;
const uint UV_SHIFT_NORMAL     = 18u;
const uint UV_SHIFT_METALROUGH = 20u;
const uint UV_SHIFT_OCCLUSION  = 22u;

vec2 selectUV(uint flags, uint shift) {
    uint idx = (flags >> shift) & 0x3u;
    return (idx == 0u) ? v_TexCoord0 : v_TexCoord1;
}

const float PI = 3.14159265359;

// ---------- PBR BRDF Functions ----------

// GGX/Trowbridge-Reitz normal distribution
float DistributionGGX(vec3 N, vec3 H, float roughness)
{
    float a  = roughness * roughness;
    float a2 = a * a;
    float NdotH  = max(dot(N, H), 0.0);
    float NdotH2 = NdotH * NdotH;

    float denom = (NdotH2 * (a2 - 1.0) + 1.0);
    denom = PI * denom * denom;

    return a2 / max(denom, 0.0000001);
}

// Schlick-GGX geometry function
float GeometrySchlickGGX(float NdotV, float roughness)
{
    float r = (roughness + 1.0);
    float k = (r * r) / 8.0;

    return NdotV / (NdotV * (1.0 - k) + k);
}

// Smith geometry function (combined)
float GeometrySmith(vec3 N, vec3 V, vec3 L, float roughness)
{
    float NdotV = max(dot(N, V), 0.0);
    float NdotL = max(dot(N, L), 0.0);
    float ggx1 = GeometrySchlickGGX(NdotV, roughness);
    float ggx2 = GeometrySchlickGGX(NdotL, roughness);

    return ggx1 * ggx2;
}

// Fresnel-Schlick approximation
vec3 FresnelSchlick(float cosTheta, vec3 F0)
{
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

// Fresnel-Schlick with roughness (for IBL — accounts for rough surfaces reducing reflections)
vec3 FresnelSchlickRoughness(float cosTheta, vec3 F0, float roughness)
{
    return F0 + (max(vec3(1.0 - roughness), F0) - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

// Calculate contribution of a single light
vec3 CalculateLight(vec3 L, vec3 radiance, vec3 V, vec3 N, vec3 albedo, float metallic, float roughness)
{
    vec3 H = normalize(V + L);

    // F0: reflectance at normal incidence (0.04 for dielectrics, albedo for metals)
    vec3 F0 = mix(vec3(0.04), albedo, metallic);

    // Cook-Torrance BRDF
    float D = DistributionGGX(N, H, roughness);
    float G = GeometrySmith(N, V, L, roughness);
    vec3  F = FresnelSchlick(max(dot(H, V), 0.0), F0);

    vec3 numerator    = D * G * F;
    float denominator = 4.0 * max(dot(N, V), 0.0) * max(dot(N, L), 0.0) + 0.0001;
    vec3 specular     = numerator / denominator;

    // Energy conservation: diffuse = (1 - specular) * (1 - metallic)
    vec3 kD = (vec3(1.0) - F) * (1.0 - metallic);

    float NdotL = max(dot(N, L), 0.0);
    return (kD * albedo / PI + specular) * radiance * NdotL;
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
// `outCascade` receives the chosen cascade index (0–3) or -1 for debug visualization.
struct ShadowResult { float shadow; int cascade; };

ShadowResult ComputeShadow(vec3 worldPos, vec3 N)
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

// ---------- Main ----------

void main()
{
    GPUMaterialData mat = materials[v_MaterialIndex];

    // --- Albedo ---
    vec4 albedo = mat.color;
    if ((mat.flags & FLAG_HAS_DIFFUSE) != 0u)
    {
        vec4 texColor = texture(globalTextures[nonuniformEXT(mat.diffuseIndex)], selectUV(mat.flags, UV_SHIFT_DIFFUSE));
        albedo *= texColor;
    }

    // --- Alpha cutoff (Cutout mode: alphaCutoff > 0; Opaque: alphaCutoff == 0) ---
    if (albedo.a < mat.alphaCutoff)
        discard;

    // --- Normal ---
    vec3 N;
    if ((mat.flags & FLAG_HAS_NORMAL) != 0u)
    {
        vec3 tangentNormal = texture(globalTextures[nonuniformEXT(mat.normalIndex)], selectUV(mat.flags, UV_SHIFT_NORMAL)).rgb;
        tangentNormal = tangentNormal * 2.0 - 1.0;
        N = normalize(v_TBN * tangentNormal);
    }
    else
    {
        N = normalize(v_Normal);
    }

    // --- Metallic / Roughness ---
    float metallic  = mat.metalness;
    float roughness = mat.roughness;
    if ((mat.flags & FLAG_HAS_METALROUGH) != 0u)
    {
        // glTF convention: G = roughness, B = metallic
        vec3 mrSample = texture(globalTextures[nonuniformEXT(mat.metalRoughIndex)], selectUV(mat.flags, UV_SHIFT_METALROUGH)).rgb;
        roughness = mrSample.g;
        metallic  = mrSample.b;
    }
    roughness = clamp(roughness, 0.04, 1.0); // Avoid zero roughness (causes NaN in GGX)

    // --- Always write entity ID for picking ---
    outEntityID = v_EntityID;

    // --- Shade mode overrides ---
    if (v_ShadeMode == 1u) { outColor = vec4(albedo.rgb, 1.0); return; }  // Unlit
    if (v_ShadeMode == 3u) { outColor = vec4(N * 0.5 + 0.5, 1.0); return; }  // Normals
    if (v_ShadeMode == 4u) {  // EntityID debug visualization
        float id = float(v_EntityID);
        vec3 idColor = vec3(fract(id * 0.123), fract(id * 0.456), fract(id * 0.789));
        outColor = vec4(idColor, 1.0);
        return;
    }

    // --- Ambient Occlusion ---
    // Material-baked AO * screen-space GTAO (epic #58). GTAO term is fetched
    // from a half-res R8 buffer via bilinear sampling; when the pass is
    // disabled at runtime, gtao.enabled == 0 and we skip the texture read.
    float ao = 1.0;
    if ((mat.flags & FLAG_HAS_OCCLUSION) != 0u)
    {
        ao = texture(globalTextures[nonuniformEXT(mat.occlusionIndex)], selectUV(mat.flags, UV_SHIFT_OCCLUSION)).r;
    }
    if (gtao.enabled != 0)
    {
        vec2 gtaoUV = gl_FragCoord.xy * gtao.invFullResolution;
        float gtaoAO = texture(gtaoTex, gtaoUV).r;
        ao *= gtaoAO;
    }

    // --- Lighting ---
    vec3 V = normalize(ubo.cameraPos - v_WorldPos);
    vec3 Lo = vec3(0.0);

    // Directional light + PCF shadow
    ShadowResult sr = ComputeShadow(v_WorldPos, N);

    {
        vec3 dirRadiance = lights.dirLight.color * lights.dirLight.intensity;
        Lo += CalculateLight(normalize(-lights.dirLight.direction), dirRadiance,
                             V, N, albedo.rgb, metallic, roughness) * sr.shadow;
    }

    // Point lights (no shadows)
    for (int i = 0; i < min(lights.numPointLights, 64); ++i)
    {
        vec3  toLight   = lights.pointLights[i].position - v_WorldPos;
        float dist      = length(toLight);
        float atten     = 1.0 / max(dist * dist, 0.0001);
        float rolloff   = pow(1.0 - clamp(dist / lights.pointLights[i].range, 0.0, 1.0), 2.0);
        vec3  ptRadiance = lights.pointLights[i].color
                         * lights.pointLights[i].intensity * atten * rolloff;
        if (dot(ptRadiance, ptRadiance) > 0.0001)
            Lo += CalculateLight(normalize(toLight), ptRadiance, V, N, albedo.rgb, metallic, roughness);
    }

    // IBL ambient lighting
    vec3 F0 = mix(vec3(0.04), albedo.rgb, metallic);
    vec3 F  = FresnelSchlickRoughness(max(dot(N, V), 0.0), F0, roughness);
    vec3 kD = (1.0 - F) * (1.0 - metallic);

    // Diffuse IBL
    vec3 irradiance = texture(irradianceMap, N).rgb;
    vec3 diffuseIBL = irradiance * albedo.rgb;

    // Specular IBL
    const float MAX_REFLECTION_LOD = 4.0;
    vec3 R = reflect(-V, N);
    vec3 prefilteredColor = textureLod(prefilteredMap, R, roughness * MAX_REFLECTION_LOD).rgb;
    vec2 brdf = texture(brdfLUT, vec2(max(dot(N, V), 0.0), roughness)).rg;
    vec3 specularIBL = prefilteredColor * (F * brdf.x + brdf.y);

    vec3 ambient = (kD * diffuseIBL + specularIBL) * ao * ubo.iblIntensity;

    vec3 color = ambient + Lo;

    // Debug viz: replace final color with the raw AO buffer so the user can
    // see the GTAO result in isolation (togglable from the Render panel).
    if (gtao.visualize != 0)
    {
        vec2 gtaoUV = gl_FragCoord.xy * gtao.invFullResolution;
        float gtaoAO = texture(gtaoTex, gtaoUV).r;
        outColor = vec4(vec3(gtaoAO), 1.0);
        outEntityID = v_EntityID;
        return;
    }

    // Debug cascade visualization: tint each cascade with a distinct color overlay.
    if (ubo.debugVisualizeCascades > 0.5 && sr.cascade >= 0)
    {
        const vec3 cascadeColors[4] = vec3[4](
            vec3(1.0, 0.2, 0.2),   // cascade 0 — red (near)
            vec3(0.2, 1.0, 0.2),   // cascade 1 — green
            vec3(0.2, 0.2, 1.0),   // cascade 2 — blue
            vec3(1.0, 1.0, 0.2)    // cascade 3 — yellow (far)
        );
        color = mix(color, cascadeColors[sr.cascade], 0.25);
    }

    outColor = vec4(color, albedo.a);
}
