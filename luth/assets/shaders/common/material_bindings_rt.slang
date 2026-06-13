// RT-set material bindings (Set 3 material SSBO, Set 4 bindless textures) + the gradient-free fetch
// policy + the FetchHitSurface / alpha-test wrappers that need those globals. Pairs with material.slang.
// Hand-pinned sets — slang#8063 forbids auto-binding the variable-count bindless heap. see arch/rendering-pipeline.md
import material;

[[vk::binding(0, 3)]] StructuredBuffer<GPUMaterialData> gMaterials;
[[vk::binding(0, 4)]] Sampler2D                         gTextures[];

public struct RayFetch : ITexFetch
{
    public float4 Sample(uint index, float2 uv) { return gTextures[NonUniformResourceIndex(index)].SampleLevel(uv, 0.0); }
}

// RT analog of EvalPbrSurface: gather geometry, decode channels (LOD 0), perturb + face the normal.
public HitSurface FetchHitSurface(GtGeomEntry* geomTable, uint customIndex, uint primIndex,
                                  float2 bary, float3x4 o2w, float3 rayDir)
{
    HitGeometry g = GatherHitGeometry(geomTable, customIndex, primIndex, bary, o2w, rayDir);
    GPUMaterialData m = gMaterials[g.materialSlot];
    RayFetch rf;
    MaterialInputs mi = EvalMaterialChannels<RayFetch>(m, g.uv0, g.uv1, rf);

    float3 N = g.ns;
    if ((m.flags & FLAG_HAS_NORMAL) != 0u && g.hasTangent)
        N = ApplyNormalMap<RayFetch>(m, g.uv0, g.uv1, g.T, cross(g.ns, g.T), g.ns, rf);
    if (dot(N, g.ng) < 0.0) N = -N;   // face shading normal to the geometric side (RT analog of gl_FrontFacing)

    HitSurface s;
    s.ns = N;  s.ng = g.ng;
    s.baseColor = mi.baseColor.rgb;
    s.emission  = mi.emissive;
    s.metalness = mi.metallic;
    s.roughness = mi.roughness;
    s.ao        = mi.occlusion;
    return s;
}

// Cutout alpha test at a rayQuery candidate hit — mirrors the diffuse-alpha decode at LOD 0.
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

// Drive a ray query to completion, confirming only alpha-passing cutout candidates (opaque instances are
// hardware-auto-confirmed and never surface as candidates).
public void RtConfirmAlphaCandidates(inout RayQuery<RAY_FLAG_NONE> q, GtGeomEntry* geomTable)
{
    while (q.Proceed())
        if (q.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE
            && AlphaTestCandidateHit(geomTable, q.CandidateInstanceID(), q.CandidatePrimitiveIndex(),
                                     q.CandidateTriangleBarycentrics()))
            q.CommitNonOpaqueTriangleHit();
}
