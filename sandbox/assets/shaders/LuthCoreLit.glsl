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

// Texture samplers
uniform sampler2D u_TexDiffuse;
uniform sampler2D u_TexNormal;
uniform sampler2D u_TexEmissive;
uniform sampler2D u_TexMetallic;
uniform sampler2D u_TexRoughness;
uniform sampler2D u_TexSpecular;

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
    vec3 albedo = texture(u_TexDiffuse, u_UVIndexDiffuse == 0 ? v_TexCoord0 : v_TexCoord1).rgb;
    vec3 normal = texture(u_TexNormal, u_UVIndexNormal == 0 ? v_TexCoord0 : v_TexCoord1).xyz;
    vec3 emissive = texture(u_TexEmissive, u_UVIndexEmissive == 0 ? v_TexCoord0 : v_TexCoord1).xyz;
    float metallic = texture(u_TexMetallic, u_UVIndexMetallic == 0 ? v_TexCoord0 : v_TexCoord1).r;
    float roughness = texture(u_TexRoughness, u_UVIndexRoughness == 0 ? v_TexCoord0 : v_TexCoord1).r;
    float ao = texture(u_TexSpecular, u_UVIndexSpecular == 0 ? v_TexCoord0 : v_TexCoord1).r;

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
