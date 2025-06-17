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
layout(binding = 0) uniform sampler2D o_Position;
layout(binding = 1) uniform sampler2D o_Normal;
layout(binding = 2) uniform sampler2D o_Albedo;
layout(binding = 3) uniform sampler2D o_MRO;
layout(binding = 4) uniform sampler2D o_ET;
layout(binding = 5) uniform sampler2D o_SSAO;

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

vec3 CalculateSubsurface(vec3 L, vec3 radiance, vec3 V, vec3 N, vec3 albedo, float metallic, float thickness) {
    vec3 transL = -L;
    float wrap = 0.5; // Wrapped lighting factor
    float transDot = max(0.0, dot(N, transL) + wrap) / (1.0 + wrap);
    float transView = max(0.0, dot(V, transL));
    
    // Combine factors with thickness
    float trans = transDot * transView * thickness; //* u_Subsurface.strength;
    
    // Apply subsurface color and energy conservation
    return mix(/*u_Subsurface.color*/vec3(1.0, 0.4, 0.4), albedo, 0.5) *  radiance *  trans * (1.0 - metallic);
}

// ========== Main Shader ==========
void main()
{
    // Retrieve G-Buffer data
    vec3 WorldPos = texture(o_Position, v_TexCoord).rgb;
    vec3 N = normalize(texture(o_Normal, v_TexCoord).rgb);
    vec4 albedoAlpha = texture(o_Albedo, v_TexCoord);
    vec3 MRO = texture(o_MRO, v_TexCoord).rgb;
    vec4 ET = texture(o_ET, v_TexCoord);
    float SSAO = texture(o_SSAO, v_TexCoord).r;
    
    vec3 albedo = albedoAlpha.rgb;
    float alpha = albedoAlpha.a;
    float metallic = MRO.r;
    float roughness = MRO.g;
    float ao = MRO.b * SSAO;
    vec3 emissive = ET.rgb;
    float thickness = ET.a;

    // View direction
    mat4 invView = inverse(view);
    vec3 viewPos = invView[3].xyz;
    vec3 V = normalize(viewPos - WorldPos);
    
    // Initialize lighting
    vec3 Lo = vec3(0.0);

    // Directional lights
    for(int i = 0; i < dirLightCount && i < MAX_DIR_LIGHTS; i++) {
        vec3 L = normalize(-dirLights[i].direction);
        vec3 radiance = dirLights[i].color * dirLights[i].intensity;
        float NdotL = dot(N, L);
    
        if (NdotL > 0.0) {
            Lo += CalculateLight(L, radiance, V, N, albedo, metallic, roughness);
        } else {
            Lo += CalculateSubsurface(L, radiance, V, N, albedo, metallic, thickness);
        }
    }

    // Point lights
    for(int i = 0; i < pointLightCount && i < MAX_POINT_LIGHTS; i++) {
        vec3 toLight = pointLights[i].position - WorldPos;
        float distance = length(toLight);
        float scaledDistance = distance / pointLights[i].range;
        
        if(scaledDistance > 1.0) continue;
        
        // Physically based attenuation
        float attenuation = 1.0 / (distance * distance);
        attenuation = clamp(1.0 - pow(scaledDistance, 4.0), 0.0, 1.0);
        vec3 L = normalize(toLight);
        vec3 radiance = pointLights[i].color * pointLights[i].intensity * attenuation;

        float NdotL = dot(N, L);
    
        if(NdotL > 0.0) {
            Lo += CalculateLight(L, radiance, V, N, albedo, metallic, roughness);
        } else {
            Lo += CalculateSubsurface(L, radiance, V, N, albedo, metallic, thickness);
        }
    }

    // Combine lighting
    vec3 ambient = vec3(0.1) * albedo * ao;// * mix(1.0, thickness, u_Subsurface.strength);
    vec3 color = ambient + Lo + emissive;

    FragColor = vec4(color, alpha);
}
