#type vertex
#version 460 core

// Vertex Attributes
layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec3 a_Normal;
layout(location = 2) in vec2 a_TexCoord0;
layout(location = 3) in vec2 a_TexCoord1;
layout(location = 4) in vec3 a_Tangent;
layout(location = 5) in ivec4 a_BoneIDs;
layout(location = 6) in vec4 a_BoneWeights;

// Outputs to Fragment Shader
layout(location = 0) out vec3 v_WorldPos;
layout(location = 1) out vec3 v_Normal;
layout(location = 2) out vec2 v_TexCoord0;
layout(location = 3) out vec2 v_TexCoord1;
layout(location = 4) out vec3 v_Tangent;
layout(location = 5) out vec3 v_Bitangent;
layout(location = 6) flat out ivec4 v_BoneIDs;
layout(location = 7) out vec4 v_BoneWeights;

// Uniform Buffers
layout(std140, binding = 0) uniform TransformUBO {
    mat4 view;
    mat4 projection;
    mat4 model;
};

uniform mat4 u_Model;

// Bone Transformations
const int MAX_BONES = 512;
const int MAX_BONE_INFLUENCE = 4;

layout(std140, binding = 2) uniform BonesUBO {
    mat4 u_BoneMatrices[MAX_BONES];
};

uniform bool u_IsSkinned;

void main()
{
    // Bone transform
    mat4 boneTransform = mat4(1.0);
    if (u_IsSkinned) {
        boneTransform  = u_BoneMatrices[a_BoneIDs.x] * a_BoneWeights.x;
        boneTransform += u_BoneMatrices[a_BoneIDs.y] * a_BoneWeights.y;
        boneTransform += u_BoneMatrices[a_BoneIDs.z] * a_BoneWeights.z;
        boneTransform += u_BoneMatrices[a_BoneIDs.w] * a_BoneWeights.w;
    }

    // Position transformation
    vec4 skinnedPosition = boneTransform * vec4(a_Position, 1.0);
    v_WorldPos = vec3(u_Model * skinnedPosition);
    gl_Position = projection * view * vec4(v_WorldPos, 1.0);
    
    // Normal/tangent transformation
    mat3 boneRotation = mat3(boneTransform);
    vec3 skinnedNormal = boneRotation * a_Normal;
    vec3 skinnedTangent = boneRotation * a_Tangent;
    
    mat3 modelNormalMatrix = transpose(inverse(mat3(u_Model)));
    v_Normal = normalize(modelNormalMatrix * skinnedNormal);
    v_Tangent = normalize(modelNormalMatrix * skinnedTangent);
    v_Bitangent = normalize(cross(v_Normal, v_Tangent));
    
    // Pass through other data
    v_TexCoord0 = a_TexCoord0;
    v_TexCoord1 = a_TexCoord1;
    v_BoneIDs = a_BoneIDs;
    v_BoneWeights = a_BoneWeights;
}




#type fragment
#version 460 core

layout(location = 0) in vec3 v_WorldPos;
layout(location = 1) in vec3 v_Normal;
layout(location = 2) in vec2 v_TexCoord0;
layout(location = 3) in vec2 v_TexCoord1;
layout(location = 4) in vec3 v_Tangent;
layout(location = 5) in vec3 v_Bitangent;
layout(location = 6) flat in ivec4 v_BoneIDs;
layout(location = 7) in vec4 v_BoneWeights;

layout(location = 0) out vec4 o_Position;   // world-space pos
layout(location = 1) out vec4 o_Normal;     // world-space normal
layout(location = 2) out vec4 o_Albedo;     // rgb=albedo, a=alpha
layout(location = 3) out vec4 o_MRO;        // r=metallic, g=roughness, b=ao, a=0
layout(location = 4) out vec4 o_ET;         // rgb=emissive, a=thickness
layout(location = 5) out vec4 o_Bone;       // rgb=boneWeights, a=0

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

vec3 BoneIdToColor(int boneId) {
    int seed = boneId * 7919; // Large prime
    return vec3(
        float(seed % 255) / 255.0,
        float((seed / 255) % 255) / 255.0,
        float((seed / 65025) % 255) / 255.0
    );
}

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
    vec3 normal;
    if (u_Maps[Normal].useTexture) {
        vec2 uv = u_Maps[Normal].uvIndex == 0 ? v_TexCoord0 : v_TexCoord1;
        mat3 TBN = mat3(normalize(v_Tangent), normalize(v_Bitangent), normalize(v_Normal));
        vec3 normalMap = texture(u_Maps[Normal].texture, uv).rgb * 2.0 - 1.0;
        normal = normalize(TBN * normalMap);
    } else {
        normal = normalize(v_Normal);
    }

    // Metallic workflow
    float metal = u_Metalness;
    if (u_Maps[Metal].useTexture) {
        vec2 uv = u_Maps[Metal].uvIndex == 0 ? v_TexCoord0 : v_TexCoord1;
        metal = texture(u_Maps[Metal].texture, uv).r;
    }

    // Roughness workflow
    float rough = u_Roughness;
    if (u_Maps[Rough].useTexture) {
        vec2 uv = u_Maps[Rough].uvIndex == 0 ? v_TexCoord0 : v_TexCoord1;
        rough = texture(u_Maps[Rough].texture, uv).r;
    }
    if (u_IsGloss == 1) rough = 1.0 - rough;

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

    // BONE WEIGTHS
    vec3 bone = vec3(0.0);
    float totalWeight = 0.0;
    bool hasValidBones = false;

    // Blend all influencing bone colors
    for (int i = 0; i < 4; i++) {
        if (v_BoneIDs[i] >= 0 && v_BoneWeights[i] > 0.0) {
            vec3 boneColor = BoneIdToColor(v_BoneIDs[i]);
            bone += boneColor * v_BoneWeights[i];
            totalWeight += v_BoneWeights[i];
            hasValidBones = true;
        }
    }

    if (hasValidBones) {
        // Normalize and enhance contrast
        bone /= totalWeight;
        bone = pow(bone, vec3(1.5)); // Gamma correction
    } else {
        // No bone influence (error color)
        bone = vec3(1.0, 0.0, 1.0); // Magenta
    }

    // write G-buffer
    o_Position  = vec4(v_WorldPos, 1.0);
    o_Normal    = vec4(normal, 1.0);
    o_Albedo    = vec4(albedo, alpha);
    o_MRO       = vec4(metal, rough, ao, 1.0);
    o_ET        = vec4(emissive, thickness);
    o_Bone      = vec4(bone, 1.0);
}