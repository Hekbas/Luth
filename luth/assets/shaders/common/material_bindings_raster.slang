// Raster-set material bindings (Set 1 bindless textures, Set 2 material SSBO) + the auto-mip fetch
// policy + the cutout alpha test for the transparent path's per-fragment shadow ray. Pairs with
// material.slang. Hand-pinned sets — slang#8063. see arch/rendering-pipeline.md
import material;

[[vk::binding(0, 1)]] Sampler2D                         gTextures[];
[[vk::binding(0, 2)]] StructuredBuffer<GPUMaterialData> gMaterials;
[[vk::binding(1, 2)]] StructuredBuffer<float4>          gMatParams;   // per-material graph constants (data, not codegen literals)

public struct RasterFetch : ITexFetch
{
    public uint paramBase;       // = materialIndex * MAT_GRAPH_STRIDE; __init zeroes it (stock decode never reads it)
    public __init() { paramBase = 0; }
    public float4 Sample(uint index, float2 uv) { return gTextures[NonUniformResourceIndex(index)].Sample(uv); }
    public float4 Param(uint slot) { return gMatParams[paramBase + slot]; }
}

// Material SSBO accessor — the buffer stays module-internal; the fragment reads its material through this.
public GPUMaterialData GetMaterial(uint index) { return gMaterials[index]; }

// Cutout alpha test for the transparent path's per-fragment sun-shadow ray (always LOD 0 — in a rayQuery).
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
