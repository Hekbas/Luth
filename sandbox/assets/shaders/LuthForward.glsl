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

// ========== Material Uniforms =========
#define Diffuse   0
#define Alpha     1
#define Normal    2
#define Metal     3
#define Rough     4
#define Specular  5
#define Oclusion  6
#define Emissive  7
#define Thickness 8

struct Map {
    bool useMap;
    bool useTexture;
    sampler2D texture;
    int uvIndex;
};

struct Subsurface {
    vec3 color;
    float strength;
    float thicknessScale;
};

uniform Map u_Maps[9];

uniform int u_RenderMode;       // 0 = Opaque, 1 = Cutout, 2 = Transparent, 3 = Fade
uniform float u_AlphaCutoff;    // For cutout mode
uniform int u_AlphaFromDiffuse;
uniform int u_IsGloss;
uniform int u_IsSingleChannel;

uniform vec4 u_Color;
uniform float u_Alpha;
uniform float u_Metalness;
uniform float u_Roughness;
uniform vec3 u_Emissive;
uniform Subsurface u_Subsurface;

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

vec3 CalculateSubsurface(vec3 L, vec3 radiance, vec3 V, vec3 N, vec3 albedo, float metallic, float thickness) {
    if (!u_Maps[Thickness].useTexture) return vec3(0.0);
    
    vec3 transL = -L;
    float wrap = 0.5; // Wrapped lighting factor
    float transDot = max(0.0, dot(N, transL) + wrap) / (1.0 + wrap);
    float transView = max(0.0, dot(V, transL));
    
    // Combine factors with thickness
    float trans = transDot * transView * thickness * u_Subsurface.strength;
    
    // Apply subsurface color and energy conservation
    return mix(u_Subsurface.color, albedo, 0.5) *  radiance *  trans * (1.0 - metallic);
}

// ========== Main Shader ==========
void main()
{
    // Diffuse color handling
    vec4 albedoRGBA = vec4(1.0);
    if (u_Maps[Diffuse].useTexture) {
        vec2 uv = u_Maps[Diffuse].uvIndex == 0 ? v_TexCoord0 : v_TexCoord1;
        albedoRGBA = texture(u_Maps[Diffuse].texture, uv);
    }
    vec3 albedo = albedoRGBA.rgb * u_Color.rgb;

    // Alpha handling
    float alpha = u_Alpha;
    if (u_AlphaFromDiffuse == 1) {
        alpha *= albedoRGBA.a;
    } else if (u_Maps[Alpha].useTexture) {
        vec2 uv = u_Maps[Alpha].uvIndex == 0 ? v_TexCoord0 : v_TexCoord1;
        alpha *= texture(u_Maps[Alpha].texture, uv).r;
    }

    // Normal mapping
    vec3 N;
    if (u_Maps[Normal].useTexture) {
        vec2 uv = u_Maps[Normal].uvIndex == 0 ? v_TexCoord0 : v_TexCoord1;
        mat3 TBN = mat3(normalize(v_Tangent), normalize(v_Bitangent), normalize(v_Normal));
        vec3 normalMap = texture(u_Maps[Normal].texture, uv).rgb * 2.0 - 1.0;
        N = normalize(TBN * normalMap);
    } else {
        N = normalize(v_Normal);
    }

    // Metallic workflow
    float metallic = u_Metalness;
    if (u_Maps[Metal].useTexture) {
        vec2 uv = u_Maps[Metal].uvIndex == 0 ? v_TexCoord0 : v_TexCoord1;
        metallic = texture(u_Maps[Metal].texture, uv).r;
    }

    // Roughness workflow
    float roughness = u_Roughness;
    if (u_Maps[Rough].useTexture) {
        vec2 uv = u_Maps[Rough].uvIndex == 0 ? v_TexCoord0 : v_TexCoord1;
        roughness = texture(u_Maps[Rough].texture, uv).r;
    }
    if (u_IsGloss == 1) roughness = 1.0 - roughness;

    // Ambient occlusion
    float ao = 1.0;
    if (u_Maps[Oclusion].useTexture) {
        vec2 uv = u_Maps[Oclusion].uvIndex == 0 ? v_TexCoord0 : v_TexCoord1;
        ao = texture(u_Maps[Oclusion].texture, uv).r;
    }

    // Emissive
    vec3 emissive = u_Emissive;
    if (u_Maps[Emissive].useTexture) {
        vec2 uv = u_Maps[Emissive].uvIndex == 0 ? v_TexCoord0 : v_TexCoord1;
        vec3 texEmissive = texture(u_Maps[Emissive].texture, uv).rgb;
        if (u_IsSingleChannel == 1) texEmissive = vec3(texEmissive.r);
        emissive *= texEmissive;
    }

    // Thickness
    float thickness = u_Subsurface.thicknessScale;
    if (u_Maps[Thickness].useTexture) {
        vec2 uv = u_Maps[Thickness].uvIndex == 0 ? v_TexCoord0 : v_TexCoord1;
        thickness *= texture(u_Maps[Thickness].texture, uv).r;
    }

    // Handle render modes
    if (u_RenderMode == 1 && alpha < u_AlphaCutoff) discard;
    if (u_RenderMode == 0) alpha = 1.0;

    // View direction
    mat4 invView = inverse(view);
    vec3 viewPos = invView[3].xyz;
    vec3 V = normalize(viewPos - v_WorldPos);

    // Lighting calculations
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
        vec3 toLight = pointLights[i].position - v_WorldPos;
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

    // Tone mapping and gamma correction
    color = ACESFilm(color);
    color = pow(color, vec3(1.0/2.2));

    FragColor = vec4(color, alpha);
}
