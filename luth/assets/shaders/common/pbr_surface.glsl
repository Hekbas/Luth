// Shared PBR surface evaluation — material fetch + albedo/normal/metal-rough/emission/AO decode.
// The raster half of the evaluate-at-surface-point seam (the RT half is common/geom_table.glsl's
// FetchHitSurface). Included by pbr.frag and the transparent-pass shaders.
//
// CONTRACT: includer enables GL_EXT_nonuniform_qualifier. Set defaults below match the engine's
// geometry pipeline layout (Set 1 bindless textures, Set 2 material SSBO); override the defines
// only for pipelines with a different set order.

#ifndef LUTH_SHADERS_COMMON_PBR_SURFACE
#define LUTH_SHADERS_COMMON_PBR_SURFACE

#ifndef PBR_TEXTURE_SET
#define PBR_TEXTURE_SET 1
#endif
#ifndef PBR_MATERIAL_SET
#define PBR_MATERIAL_SET 2
#endif

// Bindless textures (combined image-samplers) + canonical sampler array. Binding 1 is declared so
// future shader paths (per-map sampler selection) can index it; today's PBR sampling still goes
// through binding 0's per-texture baked samplers.
layout(set = PBR_TEXTURE_SET, binding = 0) uniform sampler2D globalTextures[];
layout(set = PBR_TEXTURE_SET, binding = 1) uniform sampler   bindlessSamplers[];

// Material SSBO. flags layout: bits 0-7 = HAS_* per map; bits 16-23 = per-map UV index (2 bits each).
struct GPUMaterialData {
    vec4  color;
    uint  diffuseIndex;
    uint  normalIndex;
    uint  metalRoughIndex;
    uint  occlusionIndex;
    uint  emissiveIndex;
    uint  alphaIndex;       // reserved — importer-written, unsampled (no opacity-map / spec-gloss / SSS path)
    uint  specularIndex;
    uint  thicknessIndex;
    float metalness;
    float roughness;
    float alphaCutoff;
    uint  flags;
    vec4  emissive;    // rgb = factor (linear), a = HDR strength
};

layout(std430, set = PBR_MATERIAL_SET, binding = 0) readonly buffer MaterialBuffer {
    GPUMaterialData materials[];
};

const uint FLAG_HAS_NORMAL     = (1u << 0);
const uint FLAG_HAS_METALROUGH = (1u << 1);
const uint FLAG_HAS_OCCLUSION  = (1u << 2);
const uint FLAG_HAS_DIFFUSE    = (1u << 3);
const uint FLAG_HAS_EMISSIVE   = (1u << 4);
const uint FLAG_HAS_ALPHA      = (1u << 5);
const uint FLAG_HAS_SPECULAR   = (1u << 6);
const uint FLAG_HAS_THICKNESS  = (1u << 7);

// UV index bit positions within flags (2 bits each, values 0-3)
const uint UV_SHIFT_DIFFUSE    = 16u;
const uint UV_SHIFT_NORMAL     = 18u;
const uint UV_SHIFT_METALROUGH = 20u;
const uint UV_SHIFT_OCCLUSION  = 22u;

vec2 SelectUV(uint flags, uint shift, vec2 uv0, vec2 uv1) {
    uint idx = (flags >> shift) & 0x3u;
    return (idx == 0u) ? uv0 : uv1;
}

struct PbrSurface {
    vec4  albedo;      // straight alpha (mat.color.a × diffuse texture a)
    vec3  N;           // normal-mapped, two-sided-flipped shading normal
    float metallic;
    float roughness;   // clamped to [0.04, 1] — zero roughness produces NaN in GGX
    vec3  emission;
    float materialAO;  // baked occlusion map term only (screen-space AO is the caller's business)
};

// Discards below mat.alphaCutoff immediately after the albedo fetch — BEFORE the normal/MR/emissive
// fetches — so cutout fragments don't pay the full fetch chain (matches the pre-seam pbr.frag order).
PbrSurface EvalPbrSurface(GPUMaterialData mat, vec2 uv0, vec2 uv1, mat3 TBN, vec3 vertexNormal, bool frontFacing)
{
    PbrSurface s;

    s.albedo = mat.color;
    if ((mat.flags & FLAG_HAS_DIFFUSE) != 0u)
        s.albedo *= texture(globalTextures[nonuniformEXT(mat.diffuseIndex)], SelectUV(mat.flags, UV_SHIFT_DIFFUSE, uv0, uv1));

    // Alpha cutoff (Cutout mode: alphaCutoff > 0; Opaque: alphaCutoff == 0)
    if (s.albedo.a < mat.alphaCutoff)
        discard;

    // CONTRACT: emissive radiance — MUST stay algebraically identical to common/geom_table.glsl's
    // FetchHitSurface (raster==RT). factor(linear) * strength, modulated by the emissive texture
    // (UV0 — emissive has no UV-set bit) when FLAG_HAS_EMISSIVE is set. Raster texture() vs RT
    // textureLod(.,0) is an accepted LOD asymmetry, not algebraic divergence. see arch/rendering-pipeline.md
    s.emission = mat.emissive.rgb * mat.emissive.a;
    if ((mat.flags & FLAG_HAS_EMISSIVE) != 0u)
        s.emission *= texture(globalTextures[nonuniformEXT(mat.emissiveIndex)], uv0).rgb;

    if ((mat.flags & FLAG_HAS_NORMAL) != 0u)
    {
        vec3 tangentNormal = texture(globalTextures[nonuniformEXT(mat.normalIndex)], SelectUV(mat.flags, UV_SHIFT_NORMAL, uv0, uv1)).rgb;
        tangentNormal = tangentNormal * 2.0 - 1.0;
        s.N = normalize(TBN * tangentNormal);
    }
    else
    {
        s.N = normalize(vertexNormal);
    }

    // Two-sided lighting — flip the shading normal to face the viewer on back faces (matches the RT path,
    // which faces its geometric normal against the ray). Single-sided materials cull back faces, so
    // frontFacing is always true there and this is a no-op; needed for cull-None cutout foliage.
    if (!frontFacing)
        s.N = -s.N;

    s.metallic  = mat.metalness;
    s.roughness = mat.roughness;
    if ((mat.flags & FLAG_HAS_METALROUGH) != 0u)
    {
        // glTF convention: G = roughness, B = metallic
        vec3 mrSample = texture(globalTextures[nonuniformEXT(mat.metalRoughIndex)], SelectUV(mat.flags, UV_SHIFT_METALROUGH, uv0, uv1)).rgb;
        s.roughness = mrSample.g;
        s.metallic  = mrSample.b;
    }
    s.roughness = clamp(s.roughness, 0.04, 1.0);

    s.materialAO = 1.0;
    if ((mat.flags & FLAG_HAS_OCCLUSION) != 0u)
        s.materialAO = texture(globalTextures[nonuniformEXT(mat.occlusionIndex)], SelectUV(mat.flags, UV_SHIFT_OCCLUSION, uv0, uv1)).r;

    return s;
}

#endif
