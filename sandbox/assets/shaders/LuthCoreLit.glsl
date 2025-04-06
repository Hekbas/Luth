#type vertex
#version 450

layout(binding = 0) uniform UniformBufferObject {
    mat4 model;
    mat4 view;
    mat4 proj;
} ubo;

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

void main()
{
    v_WorldPos = vec3(ubo.model * vec4(a_Position, 1.0));
    gl_Position = ubo.proj * ubo.view * vec4(v_WorldPos, 1.0);
    
    mat3 normalMatrix = transpose(inverse(mat3(ubo.model)));
    v_Normal = normalize(normalMatrix * a_Normal);
    v_Tangent = normalize(normalMatrix * a_Tangent);
    
    // Calculate bitangent using cross product with handedness
    v_Bitangent = cross(v_Normal, v_Tangent);
    v_TexCoord0 = a_TexCoord0;
    v_TexCoord1 = a_TexCoord1;
}



#type fragment
#version 450

layout(location = 0) in vec3 v_Normal;
layout(location = 1) in vec2 v_TexCoord0;
layout(location = 2) in vec2 v_TexCoord1;
layout(location = 3) in vec3 v_Tangent;
layout(location = 4) in vec3 v_Bitangent;
layout(location = 5) in vec3 v_WorldPos;

layout(location = 0) out vec4 FragColor;

layout(binding = 0) uniform UniformBufferObject {
    mat4 model;
    mat4 view;
    mat4 proj;
} ubo;

// Texture presence flags
uniform int u_HasDiffuse;
uniform int u_HasNormal;
uniform int u_HasEmissive;
uniform int u_HasMetallic;
uniform int u_HasRoughness;
uniform int u_HasSpecular;

// Texture samplers
uniform sampler2D u_TexDiffuse;
uniform sampler2D u_TexNormal;
uniform sampler2D u_TexEmissive;
uniform sampler2D u_TexMetallic;
uniform sampler2D u_TexRoughness;
uniform sampler2D u_TexSpecular;

// Default value uniforms
uniform vec3 u_DiffuseColor = vec3(0.8);
uniform vec3 u_EmissiveColor = vec3(0.0);
uniform float u_MetallicValue = 0.0;
uniform float u_RoughnessValue = 0.5;
uniform float u_SpecularValue = 0.5;

// Per texture UV Set selection
uniform int u_UVIndexDiffuse;
uniform int u_UVIndexNormal;
uniform int u_UVIndexEmissive;
uniform int u_UVIndexMetallic;
uniform int u_UVIndexRoughness;
uniform int u_UVIndexSpecular;

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

void main()
{
    // Albedo with fallback
    vec3 albedo = u_DiffuseColor;
    if (u_HasDiffuse != 0) {
        vec2 diffuseUV = u_UVIndexDiffuse == 0 ? v_TexCoord0 : v_TexCoord1;
        albedo = texture(u_TexDiffuse, diffuseUV).rgb;
    }

    // Normal with fallback to vertex normal
    vec3 normal = normalize(v_Normal);
    if (u_HasNormal != 0) {
        vec2 normalUV = u_UVIndexNormal == 0 ? v_TexCoord0 : v_TexCoord1;
        vec3 tangentNormal = texture(u_TexNormal, normalUV).xyz * 2.0 - 1.0;
        mat3 TBN = mat3(normalize(v_Tangent), normalize(v_Bitangent), normalize(v_Normal));
        normal = normalize(TBN * tangentNormal);
    }

    // Emissive with fallback
    vec3 emissive = u_EmissiveColor;
    if (u_HasEmissive != 0) {
        vec2 emissiveUV = u_UVIndexEmissive == 0 ? v_TexCoord0 : v_TexCoord1;
        emissive = texture(u_TexEmissive, emissiveUV).rgb;
    }

    // Metallic with fallback
    float metallic = u_MetallicValue;
    if (u_HasMetallic != 0) {
        vec2 metallicUV = u_UVIndexMetallic == 0 ? v_TexCoord0 : v_TexCoord1;
        metallic = texture(u_TexMetallic, metallicUV).r;
    }

    // Roughness with fallback
    float roughness = u_RoughnessValue;
    if (u_HasRoughness != 0) {
        vec2 roughnessUV = u_UVIndexRoughness == 0 ? v_TexCoord0 : v_TexCoord1;
        roughness = texture(u_TexRoughness, roughnessUV).r;
    }

    // AO/Specular with fallback
    float ao = u_SpecularValue;
    if (u_HasSpecular != 0) {
        vec2 specularUV = u_UVIndexSpecular == 0 ? v_TexCoord0 : v_TexCoord1;
        ao = texture(u_TexSpecular, specularUV).r;
    }

    // Compute world normal from normal map
    mat3 TBN = mat3(normalize(v_Tangent), normalize(v_Bitangent), normalize(v_Normal));
    vec3 N = normalize(TBN * normal);

    // View direction
    mat4 invView = inverse(ubo.view);
    vec3 viewPos = invView[3].xyz;
    vec3 V = normalize(viewPos - v_WorldPos);

    // Example light (directional)
    vec3 lightDir = normalize(vec3(1.0, 1.0, 0.5));
    vec3 lightColor = vec3(1.0);
    vec3 L = normalize(lightDir);
    vec3 H = normalize(V + L);

    vec3 F0 = mix(vec3(0.04), albedo, metallic);
    vec3 F = FresnelSchlick(max(dot(H, V), 0.0), F0);

    // BRDF components
    float NDF = DistributionGGX(N, H, roughness);
    float G = GeometrySmith(N, V, L, roughness);
    vec3 specular = (NDF * G * F) / (4.0 * max(dot(N, V), 0.0) * max(dot(N, L), 0.0) + 0.0001);

    // Diffuse and specular
    vec3 kD = (vec3(1.0) - F) * (1.0 - metallic);
    vec3 diffuse = kD * albedo / PI;

    // Combine
    vec3 Lo = (diffuse + specular) * lightColor * max(dot(N, L), 0.0);
    vec3 ambient = vec3(0.03) * albedo * ao;
    vec3 color = ambient + Lo;

    // Gamma correction
    color = pow(color + emissive, vec3(1.0 / 2.2));
    FragColor = vec4(color, 1.0);
}
