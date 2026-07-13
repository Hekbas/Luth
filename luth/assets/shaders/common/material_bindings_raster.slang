// Raster-set material bindings (Set 1 bindless textures, Set 2 material SSBO) + the auto-mip fetch
// policy + the cutout alpha test for the transparent path's per-fragment shadow ray. Pairs with
// material.slang. Hand-pinned sets (slang#8063). see arch/rendering-pipeline.md
import material;

[[vk::binding(0, 1)]] Sampler2D                         gTextures[];
[[vk::binding(0, 2)]] StructuredBuffer<GPUMaterialData> gMaterials;
[[vk::binding(1, 2)]] StructuredBuffer<float4>          gMatParams;   // per-material graph constants (data, not codegen literals)
[[vk::binding(2, 2)]] StructuredBuffer<uint>            gMatTexParams; // per-material declared-texture bindless indices

public struct RasterFetch : ITexFetch
{
    public uint   paramBase;     // = materialIndex * MAT_GRAPH_STRIDE; __init zeroes it (stock decode never reads it)
    public uint   texBase;       // = materialIndex * MAT_TEX_STRIDE; indexes gMatTexParams for declared textures
    public float3 worldPos;      // context for graph input nodes; set where graphs can decode, else __init defaults
    public float3 viewDir;
    public float  time;
    public float  ndotv;
    public float3 worldNormal;   // Triplanar blend weights; filled where a graph decodes, else __init identity
    public float3 tangentViewDir; // tangent-space view dir (parallax); __init straight-on = zero parallax
    public __init() { paramBase = 0; texBase = 0; worldPos = float3(0.0); viewDir = float3(0.0, 0.0, 1.0); time = 0.0; ndotv = 1.0; worldNormal = float3(0.0, 0.0, 1.0); tangentViewDir = float3(0.0, 0.0, 1.0); }
    public float4 Sample(uint index, float2 uv) { return gTextures[NonUniformResourceIndex(index)].Sample(uv); }
    public float4 SampleLod(uint index, float2 uv, float lod) { return gTextures[NonUniformResourceIndex(index)].SampleLevel(uv, lod); }
    public float4 Param(uint slot) { return gMatParams[paramBase + slot]; }
    public uint TexIndex(uint slot) { return gMatTexParams[texBase + slot]; }
    public float3 WorldPos() { return worldPos; }
    public float3 ViewDir()  { return viewDir; }
    public float  Time()     { return time; }
    public float  NdotV()    { return ndotv; }
    public float3 WorldNormal() { return worldNormal; }
    public float3 TangentViewDir() { return tangentViewDir; }
}

// Material SSBO accessor; the buffer stays module-internal, the fragment reads its material through this.
public GPUMaterialData GetMaterial(uint index) { return gMaterials[index]; }

// Cutout alpha test for the transparent path's per-fragment sun-shadow ray (always LOD 0, in a rayQuery).
public bool AlphaTestCandidateHit(GtGeomEntry* geomTable, uint customIndex, uint primIndex, float2 bary)
{
    GtGeomEntry ge = geomTable[customIndex];
    GPUMaterialData m = gMaterials[ge.materialSlot];
    if (m.alphaCutoff <= 0.0) return true;
    float alpha = m.color.a;
    if ((m.flags & FLAG_HAS_DIFFUSE) != 0u)
    {
        float2 uv = AlphaTestUV(geomTable, customIndex, primIndex, bary, (m.flags >> UV_SHIFT_DIFFUSE) & 3u);
        alpha *= gTextures[NonUniformResourceIndex(m.diffuseIndex)].SampleLevel(uv, 0.0).a;
    }
    return alpha >= m.alphaCutoff;
}

public void RtConfirmAlphaCandidates(inout RayQuery<RAY_FLAG_NONE> q, GtGeomEntry* geomTable)
{
    while (q.Proceed())
        if (q.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE
            && AlphaTestCandidateHit(geomTable, q.CandidateInstanceID(), q.CandidatePrimitiveIndex(),
                                     q.CandidateTriangleBarycentrics()))
            q.CommitNonOpaqueTriangleHit();
}
