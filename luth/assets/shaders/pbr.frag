#version 450
#extension GL_EXT_nonuniform_qualifier : enable

layout(location = 0) in vec3 v_WorldPos;
layout(location = 1) in vec3 v_Normal;
layout(location = 2) in vec2 v_TexCoord0;
layout(location = 6) in vec2 v_TexCoord1;
layout(location = 3) in mat3 v_TBN;    // locations 3, 4, 5

layout(location = 0) out vec4 outColor;
layout(location = 1) out uint outEntityID;

// ---------- Descriptor Sets ----------

// Set 0: Global Uniforms
layout(set = 0, binding = 0) uniform GlobalUniforms {
    mat4 viewProjection;
    mat4 view;
    mat4 projection;
    vec3 cameraPos;
    float time;
    mat4 lightSpaceMatrix;
    float shadowBias;
    float iblIntensity;
    float skyboxIntensity;
    float _pad;
} ubo;

// Set 0: IBL textures
layout(set = 0, binding = 1) uniform samplerCube irradianceMap;
layout(set = 0, binding = 2) uniform samplerCube prefilteredMap;
layout(set = 0, binding = 3) uniform sampler2D   brdfLUT;

// Set 1: Bindless Textures
layout(set = 1, binding = 0) uniform sampler2D globalTextures[];

// Set 2: Material SSBO
struct GPUMaterialData {
    vec4  color;
    uint  diffuseIndex;
    uint  normalIndex;
    uint  metalRoughIndex;
    uint  occlusionIndex;
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

layout(set = 3, binding = 1) uniform sampler2DShadow shadowMap;

// Push Constants
layout(push_constant) uniform PushConstants {
    mat4 model;
    uint materialIndex;
    uint shadeMode;
    uint entityID;
    uint boneOffset;
} pc;

// ---------- Flag Constants ----------

const uint FLAG_HAS_NORMAL     = (1u << 0);
const uint FLAG_HAS_METALROUGH = (1u << 1);
const uint FLAG_HAS_OCCLUSION  = (1u << 2);
const uint FLAG_HAS_DIFFUSE    = (1u << 3);
const uint FLAG_HAS_EMISSIVE   = (1u << 4);

// UV index bit positions within flags (2 bits each)
const uint UV_SHIFT_DIFFUSE    = 8u;
const uint UV_SHIFT_NORMAL     = 10u;
const uint UV_SHIFT_METALROUGH = 12u;
const uint UV_SHIFT_OCCLUSION  = 14u;

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

float ComputeShadow(vec3 worldPos)
{
    // Negative bias = shadows disabled
    if (ubo.shadowBias < 0.0)
        return 1.0;

    vec4 lsPos = ubo.lightSpaceMatrix * vec4(worldPos, 1.0);
    vec3 proj  = lsPos.xyz / lsPos.w;
    proj.xy    = proj.xy * 0.5 + 0.5;

    // Out of shadow frustum = fully lit
    if (proj.z < 0.0 || proj.z > 1.0 ||
        proj.x < 0.0 || proj.x > 1.0 ||
        proj.y < 0.0 || proj.y > 1.0)
        return 1.0;

    // Apply bias to reduce shadow acne
    proj.z -= ubo.shadowBias;

    // PCF 3x3 (manual texel offsets — textureOffset requires constant expressions)
    vec2 texelSize = 1.0 / vec2(textureSize(shadowMap, 0));
    float shadow = 0.0;
    for (int x = -1; x <= 1; ++x)
    {
        for (int y = -1; y <= 1; ++y)
        {
            vec3 sampleCoord = vec3(proj.xy + vec2(x, y) * texelSize, proj.z);
            shadow += texture(shadowMap, sampleCoord);
        }
    }
    return shadow / 9.0;
}

// ---------- Main ----------

void main()
{
    GPUMaterialData mat = materials[pc.materialIndex];

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
    outEntityID = pc.entityID;

    // --- Shade mode overrides ---
    if (pc.shadeMode == 1u) { outColor = vec4(albedo.rgb, 1.0); return; }  // Unlit
    if (pc.shadeMode == 3u) { outColor = vec4(N * 0.5 + 0.5, 1.0); return; }  // Normals
    if (pc.shadeMode == 4u) {  // EntityID debug visualization
        float id = float(pc.entityID);
        vec3 idColor = vec3(fract(id * 0.123), fract(id * 0.456), fract(id * 0.789));
        outColor = vec4(idColor, 1.0);
        return;
    }

    // --- Ambient Occlusion ---
    float ao = 1.0;
    if ((mat.flags & FLAG_HAS_OCCLUSION) != 0u)
    {
        ao = texture(globalTextures[nonuniformEXT(mat.occlusionIndex)], selectUV(mat.flags, UV_SHIFT_OCCLUSION)).r;
    }

    // --- Lighting ---
    vec3 V = normalize(ubo.cameraPos - v_WorldPos);
    vec3 Lo = vec3(0.0);

    // Directional light + PCF shadow
    {
        float shadow = ComputeShadow(v_WorldPos);
        vec3 dirRadiance = lights.dirLight.color * lights.dirLight.intensity;
        Lo += CalculateLight(normalize(-lights.dirLight.direction), dirRadiance,
                             V, N, albedo.rgb, metallic, roughness) * shadow;
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

    outColor = vec4(color, albedo.a);
}
