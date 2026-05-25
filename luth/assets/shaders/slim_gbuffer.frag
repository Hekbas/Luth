#version 460
#extension GL_EXT_nonuniform_qualifier : enable

// Slim G-buffer fragment shader. Outputs four attachments at viewport resolution:
//   0: normal     (RG16F)  — octahedral-encoded world-space normal, range [0, 1]
//   1: roughness  (R8)     — perceptual roughness, clamped to [0.04, 1.0]
//   2: motion     (RG16F)  — NDC delta (currNDC - prevNDC); ±2 in worst case
//   3: materialID (R16U)   — bindless material slot
// Feeds TAA (motion); downstream RT denoisers read normal+roughness, RT reflections read normal.

layout(location = 0) in vec2 v_TexCoord0;
layout(location = 1) in vec2 v_TexCoord1;
layout(location = 2) in mat3 v_TBN;    // locations 2, 3, 4
layout(location = 5) in vec4 v_CurrClip;
layout(location = 6) in vec4 v_PrevClip;
layout(location = 7) flat in uint v_MaterialIndex;

layout(location = 0) out vec2  outNormal;
layout(location = 1) out float outRoughness;
layout(location = 2) out vec2  outMotion;
layout(location = 3) out uint  outMaterialID;

// Set 1: Bindless textures (samplers baked) — same as PBR.
layout(set = 1, binding = 0) uniform sampler2D globalTextures[];

// Set 2: Material SSBO (mirrors pbr.frag's GPUMaterialData declaration).
struct GPUMaterialData {
    vec4  color;
    uint  diffuseIndex;
    uint  normalIndex;
    uint  metalRoughIndex;
    uint  occlusionIndex;
    uint  emissiveIndex;
    uint  alphaIndex;
    uint  specularIndex;
    uint  thicknessIndex;
    float metalness;
    float roughness;
    float alphaCutoff;
    uint  flags;
};
layout(std430, set = 2, binding = 0) readonly buffer MaterialBuffer {
    GPUMaterialData materials[];
};

// Flag bits — mirror pbr.frag.
const uint FLAG_HAS_NORMAL     = (1u << 0);
const uint FLAG_HAS_METALROUGH = (1u << 1);
const uint UV_SHIFT_NORMAL     = 18u;
const uint UV_SHIFT_METALROUGH = 20u;

vec2 SelectUV(uint flags, uint shift) {
    uint idx = (flags >> shift) & 0x3u;
    return (idx == 0u) ? v_TexCoord0 : v_TexCoord1;
}

// Karis 2014 octahedral encode, mapped to [0, 1] for RG16F storage.
vec2 OctEncode(vec3 n)
{
    n /= (abs(n.x) + abs(n.y) + abs(n.z));
    vec2 enc = (n.z >= 0.0) ? n.xy
                            : (1.0 - abs(n.yx)) * sign(n.xy);
    return enc * 0.5 + 0.5;
}

void main()
{
    GPUMaterialData mat = materials[v_MaterialIndex];

    // Sample the tangent-space normal map (if present) and rotate via TBN to world space.
    // Falls back to the interpolated world normal (v_TBN column 2) otherwise.
    vec3 N;
    if ((mat.flags & FLAG_HAS_NORMAL) != 0u)
    {
        vec3 tangentNormal = texture(globalTextures[nonuniformEXT(mat.normalIndex)],
                                     SelectUV(mat.flags, UV_SHIFT_NORMAL)).rgb;
        tangentNormal = tangentNormal * 2.0 - 1.0;
        N = normalize(v_TBN * tangentNormal);
    }
    else
    {
        N = normalize(v_TBN[2]);
    }
    outNormal = OctEncode(N);

    // glTF convention — roughness sits in the G channel of the metalRough texture. Clamp matches pbr.frag.
    float roughness = mat.roughness;
    if ((mat.flags & FLAG_HAS_METALROUGH) != 0u)
    {
        roughness = texture(globalTextures[nonuniformEXT(mat.metalRoughIndex)],
                            SelectUV(mat.flags, UV_SHIFT_METALROUGH)).g;
    }
    outRoughness = clamp(roughness, 0.04, 1.0);

    // Motion vector in NDC space (currNDC - prevNDC). Consumers multiply by 0.5 for UV-space
    // history reprojection (Karis14 TAA convention).
    vec2 currNDC = v_CurrClip.xy / v_CurrClip.w;
    vec2 prevNDC = v_PrevClip.xy / v_PrevClip.w;
    outMotion    = currNDC - prevNDC;

    // Material ID — R16U supports 65535 distinct slots; engine cap is 16384.
    outMaterialID = v_MaterialIndex & 0xFFFFu;
}
