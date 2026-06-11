// Shared geometry-table deref for rayQuery hit-point material lookup. Used by ReSTIR GI (secondary
// hit) + the path tracer (every bounce) — single source of truth for the GeomEntry layout so the two
// consumers can't silently drift from TlasBuilder's GPUGeometryEntry (24 B; static_assert'd C++-side).
// The table's BDA arrives in a push constant; instanceCustomIndex indexes it. Requires
// GL_EXT_buffer_reference2 + GL_EXT_nonuniform_qualifier. Set 3 = Material SSBO, Set 4 = bindless
// textures (PBR's Set 2 / Set 1 b0 remapped per-pipeline). see arch/rendering-pipeline.md

#ifndef LUTH_SHADERS_COMMON_GEOM_TABLE
#define LUTH_SHADERS_COMMON_GEOM_TABLE

// TLAS visibility masks — mirrored by TlasBuilder.cpp's instance masks. Shadow-class rays
// (sun/light visibility, fog shadows) cull to SOLID so transparent never blocks light; world-class
// rays (GI bounce, reflections, PT) trace ALL and commit glass like a surface (instances are
// FORCE_OPAQUE) — emissive glass feeds the GI bounce and shows in reflections as an unblended
// surface approximation.
#define GT_VIS_SOLID 0x01u
#define GT_VIS_ALL   0x03u

// vbuf/ibuf are the ORIGINAL full-layout buffers (positions @float 0, TexCoord0 @float 6, TexCoord1
// @float 8 — for both Vertex 52 B + SkinnedVertex 84 B). Skinned hits read bind-pose attributes (the
// hit POINT rides the deformed TLAS; only n_s/UV are bind-pose — fine for diffuse-dominant bounces).
layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer GtVerts   { float f[]; };
layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer GtIndices { uint  i[]; };
struct GtGeomEntry {
    GtVerts   vbuf;
    GtIndices ibuf;
    uint      materialSlot;
    uint      vertexStride;   // bytes
};
layout(buffer_reference, std430, buffer_reference_align = 8) readonly buffer GeomTable { GtGeomEntry e[]; };

// Mirrors GPUMaterialData (renderer/material). 80 B std430. Consumers whose pipeline layout
// already occupies sets 3/4 (the geometry layout: Light/Bones) define GT_NO_RESOURCE_DECLS and
// alias GtMaterial/gtMaterials/gtTextures onto their own declarations (see pbr_transparent.frag).
#ifndef GT_NO_RESOURCE_DECLS
struct GtMaterial {
    vec4  color;
    uint  diffuseIndex, normalIndex, metalRoughIndex, occlusionIndex;
    uint  emissiveIndex, alphaIndex, specularIndex, thicknessIndex;   // alpha/specular/thickness reserved — unsampled
    float metalness, roughness, alphaCutoff;
    uint  flags;           // bits 0-7 = HAS_* per map; bits 16-23 = per-map UV index (2 bits each)
    vec4  emissive;        // rgb = factor (linear), a = HDR strength
};
layout(std430, set = 3, binding = 0) readonly buffer GtMaterialBuffer { GtMaterial gtMaterials[]; };
layout(set = 4, binding = 0) uniform sampler2D gtTextures[];
#endif

const uint GT_FLAG_HAS_NORMAL     = (1u << 0);
const uint GT_FLAG_HAS_METALROUGH = (1u << 1);
const uint GT_FLAG_HAS_OCCLUSION  = (1u << 2);
const uint GT_FLAG_HAS_DIFFUSE    = (1u << 3);
const uint GT_FLAG_HAS_EMISSIVE   = (1u << 4);
const uint GT_UV_SHIFT_DIFFUSE    = 16u;
const uint GT_UV_SHIFT_NORMAL     = 18u;
const uint GT_UV_SHIFT_METALROUGH = 20u;
const uint GT_UV_SHIFT_OCCLUSION  = 22u;

// Surface attributes resolved at a rayQuery COMMITTED hit.
struct HitSurface {
    vec3  ns;          // SHADING normal: vertex normals (inverse-transpose) perturbed by the normal map — for the BRDF
    vec3  ng;          // GEOMETRIC (face) normal, faced against the ray — for robust ray-origin offsets + side
    vec3  baseColor;   // diffuse albedo (material color × diffuse map)
    vec3  emission;
    float metalness;
    float roughness;
    float ao;          // baked occlusion map term (1.0 if absent) — caller applies to ambient/IBL, matching raster
};

// customIndex/primIndex/bary/o2w come from rayQueryGetIntersection*EXT(rq, true); rayDir = the ray dir
// (to face the geometric normal back toward the ray).
HitSurface FetchHitSurface(GeomTable geomTable, uint customIndex, uint primIndex, vec2 bary, mat4x3 o2w, vec3 rayDir) {
    HitSurface s;
    GtGeomEntry ge = geomTable.e[customIndex];
    uint sF = ge.vertexStride >> 2u;    // stride in floats (52→13, 84→21)
    uint i0 = ge.ibuf.i[primIndex * 3u + 0u];
    uint i1 = ge.ibuf.i[primIndex * 3u + 1u];
    uint i2 = ge.ibuf.i[primIndex * 3u + 2u];

    vec3 wgt = vec3(1.0 - bary.x - bary.y, bary.x, bary.y);

    // Geometric (face) normal — cross of world-space edges (handles non-uniform scale exactly), faced
    // against the ray. Used for ray-origin offsets + to orient the shading normal's side.
    vec3 p0 = vec3(ge.vbuf.f[i0*sF + 0u], ge.vbuf.f[i0*sF + 1u], ge.vbuf.f[i0*sF + 2u]);
    vec3 p1 = vec3(ge.vbuf.f[i1*sF + 0u], ge.vbuf.f[i1*sF + 1u], ge.vbuf.f[i1*sF + 2u]);
    vec3 p2 = vec3(ge.vbuf.f[i2*sF + 0u], ge.vbuf.f[i2*sF + 1u], ge.vbuf.f[i2*sF + 2u]);
    vec3 wp0 = o2w * vec4(p0, 1.0);
    vec3 wp1 = o2w * vec4(p1, 1.0);
    vec3 wp2 = o2w * vec4(p2, 1.0);
    s.ng = normalize(cross(wp1 - wp0, wp2 - wp0));
    if (dot(s.ng, rayDir) > 0.0) s.ng = -s.ng;

    // Smooth SHADING normal — barycentric vertex normals (floats 3-5), matching pbr.frag's interpolated
    // v_Normal. Inverse-transpose object→world (pbr.vert's normalMatrix) keeps non-uniform scale raster==RT;
    // fall back to ng if absent, face to ng's side AFTER the normal-map perturb. see arch/rendering-pipeline.md
    vec3 vn0 = vec3(ge.vbuf.f[i0*sF + 3u], ge.vbuf.f[i0*sF + 4u], ge.vbuf.f[i0*sF + 5u]);
    vec3 vn1 = vec3(ge.vbuf.f[i1*sF + 3u], ge.vbuf.f[i1*sF + 4u], ge.vbuf.f[i1*sF + 5u]);
    vec3 vn2 = vec3(ge.vbuf.f[i2*sF + 3u], ge.vbuf.f[i2*sF + 4u], ge.vbuf.f[i2*sF + 5u]);
    vec3 nsObj = wgt.x * vn0 + wgt.y * vn1 + wgt.z * vn2;
    mat3 nrmM  = mat3(transpose(inverse(mat3(o2w))));   // inverse-transpose normal matrix — see pbr.vert
    vec3 nsW   = nrmM * nsObj;
    nsW = (dot(nsW, nsW) > 1.0e-12) ? normalize(nsW) : s.ng;

    // Object-space vertex tangent (floats 10-12, present in both layouts) for the normal-map TBN below.
    vec3 vt0 = vec3(ge.vbuf.f[i0*sF + 10u], ge.vbuf.f[i0*sF + 11u], ge.vbuf.f[i0*sF + 12u]);
    vec3 vt1 = vec3(ge.vbuf.f[i1*sF + 10u], ge.vbuf.f[i1*sF + 11u], ge.vbuf.f[i1*sF + 12u]);
    vec3 vt2 = vec3(ge.vbuf.f[i2*sF + 10u], ge.vbuf.f[i2*sF + 11u], ge.vbuf.f[i2*sF + 12u]);
    vec3 tanObj = wgt.x * vt0 + wgt.y * vt1 + wgt.z * vt2;

    // Barycentric UVs. TexCoord0 @float 6, TexCoord1 @float 8.
    vec2 uv0 = wgt.x*vec2(ge.vbuf.f[i0*sF + 6u], ge.vbuf.f[i0*sF + 7u])
             + wgt.y*vec2(ge.vbuf.f[i1*sF + 6u], ge.vbuf.f[i1*sF + 7u])
             + wgt.z*vec2(ge.vbuf.f[i2*sF + 6u], ge.vbuf.f[i2*sF + 7u]);
    vec2 uv1 = wgt.x*vec2(ge.vbuf.f[i0*sF + 8u], ge.vbuf.f[i0*sF + 9u])
             + wgt.y*vec2(ge.vbuf.f[i1*sF + 8u], ge.vbuf.f[i1*sF + 9u])
             + wgt.z*vec2(ge.vbuf.f[i2*sF + 8u], ge.vbuf.f[i2*sF + 9u]);

    GtMaterial m = gtMaterials[ge.materialSlot];
    s.baseColor = m.color.rgb;
    s.metalness = m.metalness;
    s.roughness = m.roughness;
    s.emission  = m.emissive.rgb * m.emissive.a;   // factor(linear) * strength — CONTRACT at the texture mod below
    if ((m.flags & GT_FLAG_HAS_DIFFUSE) != 0u) {
        vec2 uvd = (((m.flags >> GT_UV_SHIFT_DIFFUSE) & 3u) == 0u) ? uv0 : uv1;
        s.baseColor *= textureLod(gtTextures[nonuniformEXT(m.diffuseIndex)], uvd, 0.0).rgb;
    }
    // glTF metal-rough map (G = roughness, B = metallic) — matches pbr.frag. Consumed only by the path
    // tracer's full BRDF (S3); ReSTIR GI ignores metalness/roughness (diffuse-only secondary), so this
    // is an additive fetch on that path.
    if ((m.flags & GT_FLAG_HAS_METALROUGH) != 0u) {
        vec2 uvm = (((m.flags >> GT_UV_SHIFT_METALROUGH) & 3u) == 0u) ? uv0 : uv1;
        vec3 mr  = textureLod(gtTextures[nonuniformEXT(m.metalRoughIndex)], uvm, 0.0).rgb;
        s.roughness = mr.g;
        s.metalness = mr.b;
    }
    s.roughness = clamp(s.roughness, 0.04, 1.0);   // 0.04 floor — zero roughness → NaN in GGX (pbr.frag)
    // CONTRACT: emissive radiance — MUST match pbr.frag (raster==RT): factor*strength (set above),
    // modulated by the emissive texture when GT_FLAG_HAS_EMISSIVE is set. UV0 always (emissive has no
    // UV-set bit in the flags schema 16-23). see arch/rendering-pipeline.md
    if ((m.flags & GT_FLAG_HAS_EMISSIVE) != 0u) {
        s.emission *= textureLod(gtTextures[nonuniformEXT(m.emissiveIndex)], uv0, 0.0).rgb;
    }
    // Normal map — TBN (world tangent Gram-Schmidt vs nsW, B = cross(nsW, T), no handedness w) perturbs nsW,
    // mirroring pbr.vert. textureLod 0, UV-set per flags, tangent-guarded so a tangentless mesh can't poison
    // PT accumulation. see arch/rendering-pipeline.md
    if ((m.flags & GT_FLAG_HAS_NORMAL) != 0u && dot(tanObj, tanObj) > 1.0e-12) {
        vec3 T   = normalize(mat3(o2w) * tanObj);
        T        = normalize(T - dot(T, nsW) * nsW);
        vec2 uvn = (((m.flags >> GT_UV_SHIFT_NORMAL) & 3u) == 0u) ? uv0 : uv1;
        vec3 tn  = textureLod(gtTextures[nonuniformEXT(m.normalIndex)], uvn, 0.0).rgb * 2.0 - 1.0;
        nsW = normalize(mat3(T, cross(nsW, T), nsW) * tn);
    }
    // Baked occlusion (mirrors pbr_surface.glsl); the caller applies s.ao to ambient/IBL only, like raster.
    s.ao = 1.0;
    if ((m.flags & GT_FLAG_HAS_OCCLUSION) != 0u) {
        vec2 uvo = (((m.flags >> GT_UV_SHIFT_OCCLUSION) & 3u) == 0u) ? uv0 : uv1;
        s.ao = textureLod(gtTextures[nonuniformEXT(m.occlusionIndex)], uvo, 0.0).r;
    }
    // Face the (possibly perturbed) shading normal to the geometric side — RT analog of pbr.frag's
    // two-sided gl_FrontFacing flip; ng is already faced against the ray.
    if (dot(nsW, s.ng) < 0.0) nsW = -nsW;
    s.ns = nsW;
    return s;
}

// Alpha test at a rayQuery CANDIDATE hit (cutout materials). Mirrors pbr.frag: alpha = color.a *
// diffuse.a, KEEP when alpha >= alphaCutoff; opaque materials (alphaCutoff <= 0) always pass. The RT
// candidate loops confirm a hit only when this returns true. see arch/rendering-pipeline.md
bool AlphaTestCandidateHit(GeomTable geomTable, uint customIndex, uint primIndex, vec2 bary) {
    GtGeomEntry ge = geomTable.e[customIndex];
    GtMaterial  m  = gtMaterials[ge.materialSlot];
    if (m.alphaCutoff <= 0.0) return true;                  // opaque material — always passes
    float alpha = m.color.a;
    if ((m.flags & GT_FLAG_HAS_DIFFUSE) != 0u) {
        uint sF  = ge.vertexStride >> 2u;
        uint off = (((m.flags >> GT_UV_SHIFT_DIFFUSE) & 3u) == 0u) ? 6u : 8u;  // TexCoord0@6, TexCoord1@8
        uint i0 = ge.ibuf.i[primIndex*3u+0u], i1 = ge.ibuf.i[primIndex*3u+1u], i2 = ge.ibuf.i[primIndex*3u+2u];
        vec3 wgt = vec3(1.0 - bary.x - bary.y, bary.x, bary.y);
        vec2 uv = wgt.x*vec2(ge.vbuf.f[i0*sF+off], ge.vbuf.f[i0*sF+off+1u])
                + wgt.y*vec2(ge.vbuf.f[i1*sF+off], ge.vbuf.f[i1*sF+off+1u])
                + wgt.z*vec2(ge.vbuf.f[i2*sF+off], ge.vbuf.f[i2*sF+off+1u]);
        alpha *= textureLod(gtTextures[nonuniformEXT(m.diffuseIndex)], uv, 0.0).a;
    }
    return alpha >= m.alphaCutoff;
}

// Run a rayQuery to completion, confirming only alpha-passing candidate hits (cutout). Caller initializes
// `rq` WITHOUT gl_RayFlagsOpaqueEXT (so FORCE_NO_OPAQUE cutout instances surface as candidates; opaque
// instances are hardware-auto-confirmed and never appear here), then reads the committed result.
void RtConfirmAlphaCandidates(rayQueryEXT rq, GeomTable geomTable) {
    while (rayQueryProceedEXT(rq))
        if (rayQueryGetIntersectionTypeEXT(rq, false) == gl_RayQueryCandidateIntersectionTriangleEXT
            && AlphaTestCandidateHit(geomTable,
                   uint(rayQueryGetIntersectionInstanceCustomIndexEXT(rq, false)),
                   uint(rayQueryGetIntersectionPrimitiveIndexEXT(rq, false)),
                   rayQueryGetIntersectionBarycentricsEXT(rq, false)))
            rayQueryConfirmIntersectionEXT(rq);
}

#endif
