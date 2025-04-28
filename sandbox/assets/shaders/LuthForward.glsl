#type vertex
#version 460 core

layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec3 a_Normal;
layout(location = 2) in vec2 a_TexCoord0;
layout(location = 3) in vec2 a_TexCoord1;
layout(location = 4) in vec3 a_Tangent;

layout(location = 0) out vec3 v_Normal;
layout(location = 1) out vec2 v_TexCoord0;
layout(location = 2) out vec2 v_TexCoord1;
layout(location = 3) out vec3 v_Tangent;
layout(location = 4) out vec3 v_Bitangent;
layout(location = 5) out vec3 v_WorldPos;

layout(std140, binding = 0) uniform TransformUBO {
    mat4 view;
    mat4 projection;
    mat4 model;
};

void main()
{
    v_WorldPos = vec3(model * vec4(a_Position, 1.0));
    gl_Position = projection * view * vec4(v_WorldPos, 1.0);
    
    mat3 normalMatrix = transpose(inverse(mat3(model)));
    v_Normal = normalize(normalMatrix * a_Normal);
    v_Tangent = normalize(normalMatrix * a_Tangent);
    
    // Calculate bitangent using cross product with handedness
    v_Bitangent = cross(v_Normal, v_Tangent);
    v_TexCoord0 = a_TexCoord0;
    v_TexCoord1 = a_TexCoord1;
}





#type fragment
#version 460 core

// ========== Input/Output ==========
layout(location = 0) in vec3 v_Normal;
layout(location = 1) in vec2 v_TexCoord0;
layout(location = 2) in vec2 v_TexCoord1;
layout(location = 3) in vec3 v_Tangent;
layout(location = 4) in vec3 v_Bitangent;
layout(location = 5) in vec3 v_WorldPos;

layout(location = 0) out vec4 FragColor;

// ========== UBO Definitions ==========
layout(std140, binding = 0) uniform TransformUBO {
    mat4 view;
    mat4 projection;
    mat4 model;
};

#define MAX_DIR_LIGHTS 4
#define MAX_POINT_LIGHTS 16

struct DirLight {
    vec3 color;
    float intensity;
    vec3 direction;
    float padding;
};

struct PointLight {
    vec3 color;
    float intensity;
    vec3 position;
    float range;
};

layout(std140, binding = 1) uniform LightsUBO {
    int dirLightCount;
    DirLight dirLights[MAX_DIR_LIGHTS];
    
    int pointLightCount;
    PointLight pointLights[MAX_POINT_LIGHTS];
};

// ========== Material Uniforms ==========
// Texture samplers
uniform sampler2D u_TexDiffuse;
uniform sampler2D u_TexAlpha;
uniform sampler2D u_TexNormal;
uniform sampler2D u_TexEmissive;
uniform sampler2D u_TexMetallic;
uniform sampler2D u_TexRoughness;
uniform sampler2D u_TexSpecular;
uniform sampler2D u_TexOclusion;

// Per texture UV Set selection
uniform int u_UVIndexDiffuse;
uniform int u_UVIndexAlpha;
uniform int u_UVIndexNormal;
uniform int u_UVIndexEmissive;
uniform int u_UVIndexMetallic;
uniform int u_UVIndexRoughness;
uniform int u_UVIndexSpecular;
uniform int u_UVIndexOclusion;

uniform int u_RenderMode;       // 0 = Opaque, 1 = Cutout, 2 = Transparent, 3 = Fade
uniform float u_AlphaCutoff;    // For cutout mode
uniform int u_AlphaFromDiffuse;
uniform vec4 u_Color;
uniform float u_Alpha;

// ========== PBR Functions ==========
const float PI = 3.14159265359;

float DistributionGGX(vec3 N, vec3 H, float roughness) {
    float a = roughness*roughness;
    float a2 = a*a;
    float NdotH = max(dot(N, H), 0.0);
    return a2 / (PI * pow(NdotH*NdotH*(a2 - 1.0) + 1.0, 2.0));
}

float GeometrySchlickGGX(float NdotV, float roughness) {
    float r = (roughness + 1.0);
    float k = (r*r) / 8.0;
    return NdotV / (NdotV * (1.0 - k) + k);
}

float GeometrySmith(vec3 N, vec3 V, vec3 L, float roughness) {
    return GeometrySchlickGGX(max(dot(N, V), 0.0), roughness) * 
           GeometrySchlickGGX(max(dot(N, L), 0.0), roughness);
}

vec3 FresnelSchlick(float cosTheta, vec3 F0) {
    return F0 + (1.0 - F0) * pow(1.0 - cosTheta, 5.0);
}

vec3 ACESFilm(vec3 x) {
    float a = 2.51;
    float b = 0.03;
    float c = 2.43;
    float d = 0.59;
    float e = 0.14;
    return clamp((x*(a*x + b))/(x*(c*x + d) + e), 0.0, 1.0);
}

// ========== Light Calculation ==========
vec3 CalculateLight(vec3 L, vec3 radiance, vec3 V, vec3 N, vec3 albedo, float metallic, float roughness) {
    vec3 H = normalize(V + L);
    
    vec3 F0 = mix(vec3(0.04), albedo, metallic);
    vec3 F = FresnelSchlick(max(dot(H, V), 0.0), F0);
    
    float NDF = DistributionGGX(N, H, roughness);
    float G = GeometrySmith(N, V, L, roughness);
    
    vec3 numerator = NDF * G * F;
    float denominator = 4.0 * max(dot(N, V), 0.0) * max(dot(N, L), 0.0) + 0.0001;
    vec3 specular = numerator / denominator;
    
    vec3 kD = (vec3(1.0) - F) * (1.0 - metallic);
    vec3 diffuse = kD * albedo / PI;
    
    return (diffuse + specular) * radiance * max(dot(N, L), 0.0);
}

// ========== Main Shader ==========
void main()
{
    vec4 albedoRGBA = texture(u_TexDiffuse,   u_UVIndexDiffuse   == 0 ? v_TexCoord0 : v_TexCoord1).rgba;
    vec3 albedo = albedoRGBA.rgb * u_Color.rgb;
    float alpha = u_AlphaFromDiffuse == 1 ? albedoRGBA.a : texture(u_TexAlpha, u_UVIndexAlpha == 0 ? v_TexCoord0 : v_TexCoord1).r * u_Alpha;    
    vec3 normal     = texture(u_TexNormal,    u_UVIndexNormal    == 0 ? v_TexCoord0 : v_TexCoord1).rgb;
    vec3 emissive   = texture(u_TexEmissive,  u_UVIndexEmissive  == 0 ? v_TexCoord0 : v_TexCoord1).rgb;
    float metallic  = texture(u_TexMetallic,  u_UVIndexMetallic  == 0 ? v_TexCoord0 : v_TexCoord1).r;
    float roughness = texture(u_TexRoughness, u_UVIndexRoughness == 0 ? v_TexCoord0 : v_TexCoord1).r;
    //float specular  = texture(u_TexSpecular,  u_UVIndexSpecular  == 0 ? v_TexCoord0 : v_TexCoord1).r;
    float ao        = texture(u_TexOclusion,  u_UVIndexOclusion  == 0 ? v_TexCoord0 : v_TexCoord1).r;

    // Handle render modes
    if (u_RenderMode == 1 && alpha < u_AlphaCutoff) discard;
    if (u_RenderMode == 0) alpha = 1.0;

    // Normal mapping
    mat3 TBN = mat3(normalize(v_Tangent), normalize(v_Bitangent), normalize(v_Normal));
    vec3 N = normalize(TBN * (texture(u_TexNormal, u_UVIndexNormal == 0 ? v_TexCoord0 : v_TexCoord1).rgb * 2.0 - 1.0));

    // View direction
    mat4 invView = inverse(view);
    vec3 viewPos = invView[3].xyz;
    vec3 V = normalize(viewPos - v_WorldPos);

    vec3 Lo = vec3(0.0);

    // Process directional lights
    for(int i = 0; i < dirLightCount && i < MAX_DIR_LIGHTS; i++) {
        vec3 L = normalize(-dirLights[i].direction);
        vec3 radiance = dirLights[i].color * dirLights[i].intensity;
        Lo += CalculateLight(L, radiance, V, N, albedo, metallic, roughness);
    }

    // Process point lights
    for(int i = 0; i < pointLightCount && i < MAX_POINT_LIGHTS; i++) {
        vec3 toLight = pointLights[i].position - v_WorldPos;
        float distance = length(toLight);
        float scaledDistance = distance / pointLights[i].range;
        
        if(scaledDistance > 1.0) continue;
        
        // Physically based attenuation
        float attenuation = 1.0 / (distance * distance);
        attenuation *= clamp(1.0 - pow(scaledDistance, 4.0), 0.0, 1.0);
        
        vec3 L = normalize(toLight);
        vec3 radiance = pointLights[i].color * pointLights[i].intensity * attenuation;
        Lo += CalculateLight(L, radiance, V, N, albedo, metallic, roughness);
    }

    // Combine lighting
    vec3 ambient = vec3(0.04) * albedo * ao;
    vec3 color = ambient + Lo + emissive;

    // Tone mapping and gamma correction
    color = ACESFilm(color);
    color = pow(color, vec3(1.0/2.2));

    FragColor = vec4(color, alpha);
}
