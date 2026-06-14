// Temporary codegen-seam fixture (hand-written stand-in for a generated mat_graph_<uuid>.slang). EvalGraph<F>
// has the exact EvalMaterialChannels signature — a drop-in decode the consumer instantiates per tier
// (RasterFetch / RayFetch). Derivative-free, so the same body is two-tier-safe. Real graphs emit from MaterialGraphCodegen.
import material;

public MaterialInputs EvalGraph<F : ITexFetch>(GPUMaterialData m, float2 uv0, float2 uv1, F fetch)
{
    MaterialInputs mi = EvalMaterialChannels<F>(m, uv0, uv1, fetch);
    mi.baseColor.rgb *= float3(1.0, 0.0, 0.0);   // visible proof the graph decode is the one rendering
    return mi;
}
