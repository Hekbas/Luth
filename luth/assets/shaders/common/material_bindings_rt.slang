// RT-set material bindings (Set 3 material SSBO, Set 4 bindless textures) + the gradient-free fetch
// policy + the FetchHitSurface / alpha-test wrappers that need those globals. Pairs with material.slang.
// Hand-pinned sets; slang#8063 forbids auto-binding the variable-count bindless heap. see arch/rendering-pipeline.md
import material;
import mat_graph_registry;   // EvalGraphVariant: engine default (stock) or the project's generated override
import globals;              // ubo.time for graph context; every RT consumer binds Set 0 globals

[[vk::binding(0, 3)]] StructuredBuffer<GPUMaterialData> gMaterials;
[[vk::binding(1, 3)]] StructuredBuffer<float4>          gMatParams;   // shares MaterialSystem's set (binding 1); same buffer raster sees at set 2
[[vk::binding(0, 4)]] Sampler2D                         gTextures[];

public struct RayFetch : ITexFetch
{
    public uint   paramBase;     // = materialSlot * MAT_GRAPH_STRIDE; set per hit before EvalGraphVariant
    public float3 worldPos;      // graph context, filled by FetchHitSurface from the hit
    public float3 viewDir;
    public float  time;
    public float  ndotv;
    public float3 worldNormal;   // Triplanar blend weights; FetchHitSurface fills it with the smooth hit normal
    public __init() { paramBase = 0; worldPos = float3(0.0); viewDir = float3(0.0, 0.0, 1.0); time = 0.0; ndotv = 1.0; worldNormal = float3(0.0, 0.0, 1.0); }
    public float4 Sample(uint index, float2 uv) { return gTextures[NonUniformResourceIndex(index)].SampleLevel(uv, 0.0); }
    public float4 Param(uint slot) { return gMatParams[paramBase + slot]; }
    public float3 WorldPos() { return worldPos; }
    public float3 ViewDir()  { return viewDir; }
    public float  Time()     { return time; }
    public float  NdotV()    { return ndotv; }
    public float3 WorldNormal() { return worldNormal; }
}

// RT analog of EvalPbrSurface: gather geometry, decode channels (LOD 0), perturb + face the normal.
public HitSurface FetchHitSurface(GtGeomEntry* geomTable, uint customIndex, uint primIndex,
                                  float2 bary, float3x4 o2w, float3 rayDir, float3 hitPos)
{
    HitGeometry g = GatherHitGeometry(geomTable, customIndex, primIndex, bary, o2w, rayDir);
    GPUMaterialData m = gMaterials[g.materialSlot];
    RayFetch rf;
    rf.paramBase = g.materialSlot * MAT_GRAPH_STRIDE;
    rf.worldPos  = hitPos;
    rf.viewDir   = -rayDir;
    rf.time      = ubo.time;
    rf.ndotv     = saturate(dot(g.ns, -rayDir));
    rf.worldNormal = g.ns;
    MaterialInputs mi = EvalGraphVariant<RayFetch>(GraphVariant(m.flags), m, g.uv0, g.uv1, rf);

    float3 N = g.ns;
    if ((m.flags & FLAG_HAS_NORMAL) != 0u && g.hasTangent)
        N = ApplyNormalMap<RayFetch>(m, g.uv0, g.uv1, g.T, cross(g.ns, g.T) * g.tSign, g.ns, rf);
    // Graphed normal overrides on tangented meshes; tangentless RT keeps g.ns (stock-map parity).
    if (any(mi.normal != float3(0.0, 0.0, 1.0)) && g.hasTangent)
        N = ApplyTangentNormal(mi.normal, g.T, cross(g.ns, g.T) * g.tSign, g.ns);
    if (dot(N, g.ng) < 0.0) N = -N;   // face shading normal to the geometric side (RT analog of gl_FrontFacing)

    HitSurface s;
    s.ns = N;  s.ng = g.ng;
    s.baseColor = mi.baseColor.rgb * g.color;   // per-vertex color modulation (raster==RT albedo parity)
    s.emission  = mi.emissive;
    s.metalness = mi.metallic;
    s.roughness = mi.roughness;
    s.ao        = mi.occlusion;
    return s;
}

// Cutout alpha test at a rayQuery candidate hit: mirrors the diffuse-alpha decode at LOD 0.
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
