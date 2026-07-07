// Self-contained material bindings for the editor's graph-aware thumbnail / inspector preview. The
// preview submits off the frame loop, so it can't bind MaterialSystem's per-frame Set 2; it carries
// the single previewed material's data in a per-call UBO instead. Bindless reuses the engine set.
// Imports only material (common-local), so the mat_graph_registry import-resolution gotcha can't apply.
import material;

// Engine bindless texture array (Set 1 in the graph-preview pipeline layout).
[[vk::binding(0, 1)]] Sampler2D gTextures[];

// One previewed material's GPU data + its graph constants, written per call (Set 0). Field-compatible
// with GPUMaterialData's SSBO layout (all vec4/scalar -> std140 == std430) so a memcpy fills it.
public struct PreviewMaterialUBO
{
    public GPUMaterialData m;
    public float4          params[MAT_GRAPH_STRIDE];
}
[[vk::binding(0, 0)]] public ConstantBuffer<PreviewMaterialUBO> gPreview;

// Fetch policy over the preview UBO + bindless. paramBase stays 0; the UBO holds exactly this
// material's slots, so the generated EvalGraph reads gPreview.params[k] directly.
public struct PreviewFetch : ITexFetch
{
    public uint   paramBase;
    public float3 worldPos;      // canned context: thumbnails are static bakes (Time frozen at 0)
    public float3 viewDir;
    public float  time;
    public float  ndotv;
    public float3 worldNormal;   // canned like worldPos; Triplanar previews flat (stable), same as WorldPos nodes
    public __init() { paramBase = 0; worldPos = float3(0.0); viewDir = float3(0.0, 0.0, 1.0); time = 0.0; ndotv = 1.0; worldNormal = float3(0.0, 0.0, 1.0); }
    public float4 Sample(uint index, float2 uv) { return gTextures[NonUniformResourceIndex(index)].Sample(uv); }
    public float4 Param(uint slot) { return gPreview.params[slot]; }
    public float3 WorldPos() { return worldPos; }
    public float3 ViewDir()  { return viewDir; }
    public float  Time()     { return time; }
    public float  NdotV()    { return ndotv; }
    public float3 WorldNormal() { return worldNormal; }
}

// Vertex->fragment varyings the generated thumbnail_graph consumer reads (matches thumbnail_graph.vert).
public struct PreviewVaryings
{
    [[vk::location(0)]] public float3 normal;
    [[vk::location(1)]] public float3 tangent;
    [[vk::location(2)]] public float2 uv0;
    [[vk::location(3)]] public float2 uv1;
}
