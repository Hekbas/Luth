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

void main()
{
    vec3 albedo =     texture(u_TexDiffuse,   u_UVIndexDiffuse   == 0 ? v_TexCoord0 : v_TexCoord1).rgb;
    vec3 normal =     texture(u_TexNormal,    u_UVIndexNormal    == 0 ? v_TexCoord0 : v_TexCoord1).xyz;
    vec3 emissive =   texture(u_TexEmissive,  u_UVIndexEmissive  == 0 ? v_TexCoord0 : v_TexCoord1).xyz;
    float metallic =  texture(u_TexMetallic,  u_UVIndexMetallic  == 0 ? v_TexCoord0 : v_TexCoord1).r;
    float roughness = texture(u_TexRoughness, u_UVIndexRoughness == 0 ? v_TexCoord0 : v_TexCoord1).r;
    float ao =        texture(u_TexSpecular,  u_UVIndexSpecular  == 0 ? v_TexCoord0 : v_TexCoord1).r;

    // Calculate which column we're in (0-5)
    int column = int((gl_FragCoord.x / 1691.0/*u_ScreenSize.x*/) * 6.0);
    column = clamp(column, 0, 5);

    // Select texture based on column
    vec3 color;
    switch(column) {
        case 0: color = albedo;             break;  // Diffuse 
        case 1: color = normal * 0.5 + 0.5; break;  // Normal (mapped to [0,1])
        case 2: color = emissive;           break;  // Emissive
        case 3: color = vec3(metallic);     break;  // Metallic
        case 4: color = vec3(roughness);    break;  // Roughness
        case 5: color = vec3(ao);           break;  // AO
        default:color = vec3(0.0);                  // Fallback
    }
    
    FragColor = vec4(color, 1.0);
}
