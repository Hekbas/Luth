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

layout(location = 0) in vec2 v_TexCoord;

// G-Buffer Inputs
layout(binding = 0) uniform sampler2D gPosition;
layout(binding = 1) uniform sampler2D gNormal;
layout(binding = 2) uniform sampler2D gAlbedo;
layout(binding = 3) uniform sampler2D gMRAO;

layout(location = 0) out vec4 o_Color;

// Lighting Uniforms
struct DirectionalLight {
    vec3 direction;
    vec3 color;
    float intensity;
};

// layout(binding = 4) uniform LightingUBO {
//     vec3 viewPos;
//     DirectionalLight dirLight;
//     vec3 ambientColor;
//     float ambientIntensity;
// } ubo;

layout(binding = 0) uniform UniformBufferObject {
    mat4 model;
    mat4 view;
    mat4 proj;
} ubo;

// PBR Functions
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

// Test values
vec3 testViewPos = vec3(0.0, 0.0, 5.0);
vec3 testLightDir = normalize(vec3(1.0, 1.0, 0.5));
vec3 testLightColor = vec3(1.0, 1.0, 1.0);
float testLightIntensity = 1.0;
vec3 testAmbient = vec3(1.0, 1.0, 1.0);
float testAmbientIntensity = 0.04;


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
    
    // Calculate F0 for Fresnel
    vec3 F0 = vec3(0.04); 
    F0 = mix(F0, albedo, metallic);

    // Directional Light calculation
    vec3 L = normalize(testLightDir);
    vec3 H = normalize(V + L);
    
    // Radiance
    vec3 radiance = testLightColor * testLightIntensity;

    // Cook-Torrance BRDF
    float NDF = DistributionGGX(N, H, roughness);
    float G = GeometrySmith(N, V, L, roughness);
    vec3 F = FresnelSchlick(max(dot(H, V), 0.0), F0);
    
    vec3 numerator = NDF * G * F;
    float denominator = 4.0 * max(dot(N, V), 0.0) * max(dot(N, L), 0.0);
    vec3 specular = numerator / max(denominator, 0.001);

    vec3 kS = F;
    vec3 kD = (vec3(1.0) - kS) * (1.0 - metallic);

    float NdotL = max(dot(N, L), 0.0);
    
    // Combine lighting
    vec3 Lo = (kD * albedo / PI + specular) * radiance * NdotL;

    // Ambient lighting
    vec3 ambient = testAmbient * testAmbientIntensity * albedo * ao;
    
    vec3 color = ambient + Lo;

    // Apply simple tonemapping and gamma correction
    color = color / (color + vec3(1.0));
    color = pow(color, vec3(1.0/2.2));

    o_Color = vec4(color, alpha);
}
