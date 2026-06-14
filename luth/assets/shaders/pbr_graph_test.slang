// Temporary codegen-seam fixture: the generated per-material consumer shape. Identical to pbr.slang except
// the decode swaps EvalMaterialChannels → the graph's EvalGraph. Proves a per-material decode swap compiles +
// renders through the shared PbrShadeSurface. Replaced by real MaterialGraphCodegen output. see arch/rendering-pipeline.md
import globals;
import material;
import material_bindings_raster;
import pbr_shade;
import mat_graph_test;

[shader("fragment")]
FOut main(VIn i, bool frontFacing : SV_IsFrontFace, float4 fragCoord : SV_Position)
{
    GPUMaterialData m = GetMaterial(i.materialIndex);
    RasterFetch rf;
    MaterialInputs mi = EvalGraph<RasterFetch>(m, i.uv0, i.uv1, rf);
    return PbrShadeSurface(mi, m, i, frontFacing, fragCoord);
}
