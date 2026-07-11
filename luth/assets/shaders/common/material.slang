// Bounded material surface + the two-tier evaluate-at-surface-point seam, shared by the raster
// fragment path and the rayQuery-in-compute RT path. Replaces the hand-mirrored GLSL triplet
// (pbr_surface.glsl EvalPbrSurface + geom_table.glsl FetchHitSurface + the C++ GPUMaterialData echo).
// Resource-agnostic: bindings (raster set 1/2 vs RT set 4/3) + the ITexFetch impls live in
// material_bindings_{raster,rt}.slang so one decode serves both set layouts. see arch/rendering-pipeline.md

// 80 B std430, field-identical to C++ Material.h GPUMaterialData (MaterialLayoutGuard cross-checks).
public struct GPUMaterialData
{
    public float4 color;
    public uint   diffuseIndex, normalIndex, metalRoughIndex, occlusionIndex;
    public uint   emissiveIndex, alphaIndex, specularIndex, thicknessIndex;  // alpha/spec/thickness reserved, unsampled
    public float  metalness, roughness, alphaCutoff;
    public uint   flags;          // bits 0-7 = HAS_* per map; bits 16-23 = per-map UV index (2 bits each)
    public float4 emissive;       // rgb = factor (linear), a = HDR strength
}

public static const uint FLAG_HAS_NORMAL     = (1u << 0);
public static const uint FLAG_HAS_METALROUGH = (1u << 1);
public static const uint FLAG_HAS_OCCLUSION  = (1u << 2);
public static const uint FLAG_HAS_DIFFUSE    = (1u << 3);
public static const uint FLAG_HAS_EMISSIVE   = (1u << 4);
public static const uint UV_SHIFT_DIFFUSE    = 16u;
public static const uint UV_SHIFT_NORMAL     = 18u;
public static const uint UV_SHIFT_METALROUGH = 20u;
public static const uint UV_SHIFT_OCCLUSION  = 22u;

public float2 SelectUV(uint flags, uint shift, float2 uv0, float2 uv1)
{
    return (((flags >> shift) & 0x3u) == 0u) ? uv0 : uv1;
}

// bits 8-15: node-graph eval variant (0 = stock channel decode). The RT megakernel's FetchHitSurface
// dispatches on this through EvalGraphVariant; raster binds a per-material shader, so it never reads it.
public static const uint GRAPH_VARIANT_SHIFT = 8u;
public uint GraphVariant(uint flags) { return (flags >> GRAPH_VARIANT_SHIFT) & 0xFFu; }

// Per-material graph-param stride (float4 slots/material). invariant: matches C++ Material.h MAT_GRAPH_STRIDE;
// the fetch policy's paramBase = materialIndex * MAT_GRAPH_STRIDE indexes gMatParams, so drift cross-corrupts.
public static const uint MAT_GRAPH_STRIDE = 16u;

// Bounded surface (Filament model, no escape hatch); derivative/screen-space ops excluded by construction.
public struct MaterialInputs
{
    public float4 baseColor;   // straight alpha (color.a x diffuse.a); RT ignores .a
    public float3 normal;      // tangent-space normal, [-1,1], identity (0,0,1); caller applies the TBN + facing
    public float  metallic;
    public float  roughness;   // clamped [0.04, 1]; zero roughness -> NaN in GGX
    public float  occlusion;   // baked AO map term (screen-space AO is the caller's business)
    public float3 emissive;
}

// The two-tier seam: raster auto-mips (derivatives), RT samples LOD 0 (no derivatives at a ray hit).
public interface ITexFetch
{
    float4 Sample(uint index, float2 uv);
    // Per-material graph constant, paramBase-relative slot. Stock decode never calls it; generated
    // EvalGraph bodies read their canonical const slots through it (gMatParams, bound per tier).
    float4 Param(uint slot);
    // Per-tier surface context for graph input nodes (WorldPos/ViewDir/Time/Fresnel). Stock decode never
    // calls these; decode sites that can host graphs fill them (__init zeroes cover every stock site).
    float3 WorldPos();
    float3 ViewDir();   // toward the camera (RT: -rayDir)
    float  Time();
    float  NdotV();     // vertex/smooth normal (pre-normal-map) . ViewDir; graphed normals never feed back
    float3 WorldNormal();   // world-space smooth normal, pre-perturb; Triplanar blend weights
}

// Channel decode: bit-identical between tiers modulo the Sample-vs-SampleLevel inside F. Leaves .normal
// at identity (0,0,1); the caller applies the normal map / graph tangent normal through the TBN + facing.
public MaterialInputs EvalMaterialChannels<F : ITexFetch>(GPUMaterialData m, float2 uv0, float2 uv1, F fetch)
{
    MaterialInputs mi;

    mi.baseColor = m.color;
    if ((m.flags & FLAG_HAS_DIFFUSE) != 0u)
        mi.baseColor *= fetch.Sample(m.diffuseIndex, SelectUV(m.flags, UV_SHIFT_DIFFUSE, uv0, uv1));

    // CONTRACT: emissive radiance = factor(linear) x strength, modulated by the emissive texture (UV0,
    // emissive has no UV-set bit). Identical on both tiers. see arch/rendering-pipeline.md
    mi.emissive = m.emissive.rgb * m.emissive.a;
    if ((m.flags & FLAG_HAS_EMISSIVE) != 0u)
        mi.emissive *= fetch.Sample(m.emissiveIndex, uv0).rgb;

    mi.metallic  = m.metalness;
    mi.roughness = m.roughness;
    if ((m.flags & FLAG_HAS_METALROUGH) != 0u)
    {
        // glTF convention: G = roughness, B = metallic.
        float3 mr = fetch.Sample(m.metalRoughIndex, SelectUV(m.flags, UV_SHIFT_METALROUGH, uv0, uv1)).rgb;
        mi.roughness = mr.g;
        mi.metallic  = mr.b;
    }
    mi.roughness = clamp(mi.roughness, 0.04, 1.0);

    mi.occlusion = 1.0;
    if ((m.flags & FLAG_HAS_OCCLUSION) != 0u)
        mi.occlusion = fetch.Sample(m.occlusionIndex, SelectUV(m.flags, UV_SHIFT_OCCLUSION, uv0, uv1)).r;

    mi.normal = float3(0.0, 0.0, 1.0);
    return mi;
}

// Tangent-space normal -> world via the surface basis (explicit column combo, layout-independent).
// Identity (0,0,1) returns N exactly, so a flat/unperturbed tangent normal is a no-op.
public float3 ApplyTangentNormal(float3 tn, float3 T, float3 B, float3 N)
{
    return normalize(T * tn.x + B * tn.y + N * tn.z);
}

// Decode a tangent-space normal from a sampled texel: XY from the stored RG, Z reconstructed. Makes BC5
// (RG only, B samples 0) and RGB normal maps decode identically -- valid for any unit-length map.
public float3 DecodeTangentNormal(float3 sampled)
{
    float2 xy = sampled.xy * 2.0 - 1.0;
    float z = sqrt(saturate(1.0 - dot(xy, xy)));
    return float3(xy, z);
}

// Normal-map perturb (decode + basis); caller guards HAS_NORMAL + tangent.
public float3 ApplyNormalMap<F : ITexFetch>(GPUMaterialData m, float2 uv0, float2 uv1,
                                            float3 T, float3 B, float3 N, F fetch)
{
    float3 tn = DecodeTangentNormal(fetch.Sample(m.normalIndex, SelectUV(m.flags, UV_SHIFT_NORMAL, uv0, uv1)).rgb);
    return ApplyTangentNormal(tn, T, B, N);
}

// Affine remap with (inMin, inMax, outMin, outMax) packed in p: a Remap graph node lowers to this over a
// per-material param, so editing its range is data, not a recompile. Degenerate range collapses to 0 scale.
public float4 RemapApply(float4 v, float4 p)
{
    float denom = p.y - p.x;
    float scale = (abs(denom) < 1.0e-6) ? 0.0 : (p.w - p.z) / denom;
    float bias  = p.z - p.x * scale;
    return v * scale + float4(bias);
}

// ---- RT geometry-table deref ----
// TLAS visibility masks mirror TlasBuilder.cpp's instance masks. Shadow-class rays cull to SOLID (glass
// never blocks light); world-class rays trace ALL and commit glass as a surface. see arch/rendering-pipeline.md
public static const uint GT_VIS_SOLID = 0x01u;
public static const uint GT_VIS_ALL   = 0x03u;

// Raw BDA pointers (buffer_reference2 equiv). Mirrors TlasBuilder's GPUGeometryEntry: 24 B, static_assert'd C++-side.
public struct GtGeomEntry
{
    public float* vbuf;
    public uint*  ibuf;
    public uint   materialSlot;
    public uint   vertexStride;   // bytes
}

// Surface geometry at a rayQuery COMMITTED hit, pre-normal-map (the wrapper perturbs ns afterward).
public struct HitGeometry
{
    public float3 ng;          // geometric (face) normal, faced against the ray
    public float3 ns;          // smooth shading normal (inverse-transpose), pre-perturb
    public float3 T;           // world tangent, Gram-Schmidt vs ns (valid iff hasTangent)
    public float  tSign;       // bitangent handedness sign (Vertex tangent.w); constant per triangle
    public float2 uv0, uv1;
    public float3 color;       // per-vertex color (linear RGB), barycentric; multiplied into baseColor
    public uint   materialSlot;
    public bool   hasTangent;
}

// Resolved RT hit surface: same field shape the GLSL FetchHitSurface returned, so RT consumers port 1:1.
public struct HitSurface
{
    public float3 ns, ng;
    public float3 baseColor;
    public float3 emission;
    public float  metalness, roughness, ao;
}

// Slang has no inverse() (HLSL lineage); cofactor 3x3, feeds only the inverse-transpose normal matrix.
float3x3 Inverse3x3(float3x3 m)
{
    float3 a = float3(m[0][0], m[1][0], m[2][0]);
    float3 b = float3(m[0][1], m[1][1], m[2][1]);
    float3 c = float3(m[0][2], m[1][2], m[2][2]);
    float3 r0 = cross(b, c);
    float3 r1 = cross(c, a);
    float3 r2 = cross(a, b);
    float invDet = 1.0 / dot(a, r0);
    return float3x3(r0 * invDet, r1 * invDet, r2 * invDet);
}

// Geometry half of the old FetchHitSurface: positions/normals/tangent/UVs from the barycentric BDA fetch.
public HitGeometry GatherHitGeometry(GtGeomEntry* geomTable, uint customIndex, uint primIndex,
                                     float2 bary, float3x4 o2w, float3 rayDir)
{
    HitGeometry g;
    GtGeomEntry ge = geomTable[customIndex];
    g.materialSlot = ge.materialSlot;
    uint sF = ge.vertexStride >> 2u;     // stride in floats (72->18, 104->26)
    uint i0 = ge.ibuf[primIndex * 3u + 0u];
    uint i1 = ge.ibuf[primIndex * 3u + 1u];
    uint i2 = ge.ibuf[primIndex * 3u + 2u];

    float3 wgt = float3(1.0 - bary.x - bary.y, bary.x, bary.y);

    // Geometric (face) normal: cross of world-space edges, faced against the ray.
    float3 p0 = float3(ge.vbuf[i0*sF + 0u], ge.vbuf[i0*sF + 1u], ge.vbuf[i0*sF + 2u]);
    float3 p1 = float3(ge.vbuf[i1*sF + 0u], ge.vbuf[i1*sF + 1u], ge.vbuf[i1*sF + 2u]);
    float3 p2 = float3(ge.vbuf[i2*sF + 0u], ge.vbuf[i2*sF + 1u], ge.vbuf[i2*sF + 2u]);
    float3 wp0 = mul(o2w, float4(p0, 1.0));
    float3 wp1 = mul(o2w, float4(p1, 1.0));
    float3 wp2 = mul(o2w, float4(p2, 1.0));
    g.ng = normalize(cross(wp1 - wp0, wp2 - wp0));
    if (dot(g.ng, rayDir) > 0.0) g.ng = -g.ng;

    // Smooth shading normal: barycentric vertex normals (floats 3-5), inverse-transpose object->world.
    float3 vn0 = float3(ge.vbuf[i0*sF + 3u], ge.vbuf[i0*sF + 4u], ge.vbuf[i0*sF + 5u]);
    float3 vn1 = float3(ge.vbuf[i1*sF + 3u], ge.vbuf[i1*sF + 4u], ge.vbuf[i1*sF + 5u]);
    float3 vn2 = float3(ge.vbuf[i2*sF + 3u], ge.vbuf[i2*sF + 4u], ge.vbuf[i2*sF + 5u]);
    float3 nsObj = wgt.x * vn0 + wgt.y * vn1 + wgt.z * vn2;
    float3x3 nrmM = transpose(Inverse3x3((float3x3)o2w));
    g.ns = mul(nrmM, nsObj);
    g.ns = (dot(g.ns, g.ns) > 1.0e-12) ? normalize(g.ns) : g.ng;

    // Object-space vertex tangent (floats 10-12) -> world, Gram-Schmidt vs ns. Tangentless meshes skip
    // the normal map (guard) so they can't poison accumulation.
    float3 vt0 = float3(ge.vbuf[i0*sF + 10u], ge.vbuf[i0*sF + 11u], ge.vbuf[i0*sF + 12u]);
    float3 vt1 = float3(ge.vbuf[i1*sF + 10u], ge.vbuf[i1*sF + 11u], ge.vbuf[i1*sF + 12u]);
    float3 vt2 = float3(ge.vbuf[i2*sF + 10u], ge.vbuf[i2*sF + 11u], ge.vbuf[i2*sF + 12u]);
    float3 tanObj = wgt.x * vt0 + wgt.y * vt1 + wgt.z * vt2;
    g.hasTangent = dot(tanObj, tanObj) > 1.0e-12;
    g.tSign = ge.vbuf[i0*sF + 13u];   // handedness sign, constant per triangle -> single-vertex read
    float3 Tw = normalize(mul((float3x3)o2w, tanObj));
    g.T = normalize(Tw - dot(Tw, g.ns) * g.ns);

    // Barycentric UVs. TexCoord0 @float 6, TexCoord1 @float 8.
    g.uv0 = wgt.x * float2(ge.vbuf[i0*sF + 6u], ge.vbuf[i0*sF + 7u])
          + wgt.y * float2(ge.vbuf[i1*sF + 6u], ge.vbuf[i1*sF + 7u])
          + wgt.z * float2(ge.vbuf[i2*sF + 6u], ge.vbuf[i2*sF + 7u]);
    g.uv1 = wgt.x * float2(ge.vbuf[i0*sF + 8u], ge.vbuf[i0*sF + 9u])
          + wgt.y * float2(ge.vbuf[i1*sF + 8u], ge.vbuf[i1*sF + 9u])
          + wgt.z * float2(ge.vbuf[i2*sF + 8u], ge.vbuf[i2*sF + 9u]);

    // Per-vertex color (floats 14-16), barycentric; alpha (float 17) unused since RT baseColor is RGB.
    // Color sits at float 14 in both the 18-float Vertex and 26-float SkinnedVertex layouts (shared prefix).
    float3 vc0 = float3(ge.vbuf[i0*sF + 14u], ge.vbuf[i0*sF + 15u], ge.vbuf[i0*sF + 16u]);
    float3 vc1 = float3(ge.vbuf[i1*sF + 14u], ge.vbuf[i1*sF + 15u], ge.vbuf[i1*sF + 16u]);
    float3 vc2 = float3(ge.vbuf[i2*sF + 14u], ge.vbuf[i2*sF + 15u], ge.vbuf[i2*sF + 16u]);
    g.color = wgt.x * vc0 + wgt.y * vc1 + wgt.z * vc2;
    return g;
}

// Barycentric UV for the cutout alpha test (TexCoord0@6 / TexCoord1@8): the slice the candidate loop needs.
public float2 AlphaTestUV(GtGeomEntry* geomTable, uint customIndex, uint primIndex, float2 bary, uint uvSet)
{
    GtGeomEntry ge = geomTable[customIndex];
    uint sF  = ge.vertexStride >> 2u;
    uint off = (uvSet == 0u) ? 6u : 8u;
    uint i0 = ge.ibuf[primIndex*3u+0u], i1 = ge.ibuf[primIndex*3u+1u], i2 = ge.ibuf[primIndex*3u+2u];
    float3 wgt = float3(1.0 - bary.x - bary.y, bary.x, bary.y);
    return wgt.x * float2(ge.vbuf[i0*sF+off], ge.vbuf[i0*sF+off+1u])
         + wgt.y * float2(ge.vbuf[i1*sF+off], ge.vbuf[i1*sF+off+1u])
         + wgt.z * float2(ge.vbuf[i2*sF+off], ge.vbuf[i2*sF+off+1u]);
}
