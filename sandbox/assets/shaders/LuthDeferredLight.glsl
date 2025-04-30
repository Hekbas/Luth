#type vertex
#version 460 core

layout(location = 0) in vec2 a_Position;

layout(location = 0) out vec2 v_TexCoord;

void main()
{
    gl_Position = vec4(a_Position, 0.0, 1.0);
    v_TexCoord = a_Position * 0.5 + 0.5; // Convert to 0-1 UV
}





#type fragment
#version 460 core

// ========== Input/Output ==========
layout(location = 0) in vec2 v_TexCoord;

// G-Buffer Inputs
layout(binding = 0) uniform sampler2D gPosition;
layout(binding = 1) uniform sampler2D gNormal;
layout(binding = 2) uniform sampler2D gAlbedo;
layout(binding = 3) uniform sampler2D gMRAO;

layout(location = 0) out vec4 o_Color;

// ========== UBO Definitions ==========
layout(std140, binding = 0) uniform TransformUBO {
    mat4 view;
    mat4 projection;
    mat4 model;
} ubo;

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
} lights;

// ========== PBR Functions ==========
const float PI = 3.14159265359;

float DistributionGGX(vec3 N, vec3 H, float roughness) {
    float a = roughness * roughness;
    float a2 = a * a;
    float NdotH = max(dot(N, H), 0.0);
    return a2 / (PI * pow(NdotH * NdotH * (a2 - 1.0) + 1.0, 2.0));
}

float GeometrySchlickGGX(float NdotV, float roughness) {
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;
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
    // Retrieve G-Buffer data
    vec3 WorldPos = texture(gPosition, v_TexCoord).rgb;
    vec3 N = normalize(texture(gNormal, v_TexCoord).rgb);
    vec4 albedoData = texture(gAlbedo, v_TexCoord);
    vec3 MRAO = texture(gMRAO, v_TexCoord).rgb;
    
    vec3 albedo = albedoData.rgb;
    float alpha = albedoData.a;
    float metallic = MRAO.r;
    float roughness = MRAO.g;
    float ao = MRAO.b;

    // View direction
    mat4 invView = inverse(ubo.view);
    vec3 viewPos = invView[3].xyz;
    vec3 V = normalize(viewPos - WorldPos);
    
    // Initialize lighting
    vec3 Lo = vec3(0.0);

    // Calculate directional lights
    for(int i = 0; i < lights.dirLightCount; i++) {
        DirLight light = lights.dirLights[i];
        vec3 L = normalize(-light.direction); // Direction from light to surface
        vec3 radiance = light.color * light.intensity;
        Lo += CalculateLight(L, radiance, V, N, albedo, metallic, roughness);
    }

    // Calculate point lights
    for(int i = 0; i < lights.pointLightCount; i++) {
        PointLight light = lights.pointLights[i];
        vec3 L = normalize(light.position - WorldPos);
        float distance = length(light.position - WorldPos);
        float scaledDistance = distance / light.range;
        
        if(scaledDistance > 1.0) continue;

        // Physically based attenuation
        float attenuation = 1.0 / (distance * distance);
        attenuation = clamp(1.0 - pow(scaledDistance, 4.0), 0.0, 1.0);

        vec3 radiance = light.color * light.intensity * attenuation;
        Lo += CalculateLight(L, radiance, V, N, albedo, metallic, roughness);
    }

    // Ambient lighting
    vec3 ambient = vec3(0.03) * albedo * ao;
    
    vec3 color = ambient + Lo;

    // Apply tonemapping and gamma correction
    color = ACESFilm(color);
    color = pow(color, vec3(1.0/2.2));

    o_Color = vec4(color, alpha);
}
