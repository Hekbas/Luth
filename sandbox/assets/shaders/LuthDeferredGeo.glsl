#type vertex
#version 450

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

layout(binding = 0) uniform UniformBufferObject {
    mat4 model;
    mat4 view;
    mat4 proj;
} ubo;

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

layout(location = 0) out vec4 gPosition;
layout(location = 1) out vec3 gNormal;
layout(location = 2) out vec4 gAlbedo;
layout(location = 3) out vec3 gMRAO;

layout(binding = 0) uniform UniformBufferObject {
    mat4 model;
    mat4 view;
    mat4 proj;
} ubo;

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

    // Compute world normal from normal map
    mat3 TBN = mat3(normalize(v_Tangent), normalize(v_Bitangent), normalize(v_Normal));
    vec3 N = normalize(TBN * (normal * 2.0 - 1.0));


    gPosition = vec4(v_WorldPos, gl_FragCoord.z);
    gNormal = N;
    gAlbedo = vec4(albedo, alpha);
    
    // Material properties
    gMRAO = vec3(metallic, roughness, ao);
}
