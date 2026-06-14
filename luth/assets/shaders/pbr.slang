// Forward PBR opaque fragment — stock bounded-surface decode (EvalMaterialChannels, RasterFetch tier),
// then the shared PbrShadeSurface shading body (pbr_shade.slang). A node-graph material instead emits a
// generated pbr_graph_<uuid>.slang that swaps only the decode call. see arch/rendering-pipeline.md
import globals;
import material;
import material_bindings_raster;   // GetMaterial + RasterFetch (Set 1 bindless, Set 2 material SSBO)
import pbr_shade;                  // VIn / FOut / PbrShadeSurface (+ the Set 0/3 screen-space bindings)

[shader("fragment")]
FOut main(VIn i, bool frontFacing : SV_IsFrontFace, float4 fragCoord : SV_Position)
{
    GPUMaterialData m = GetMaterial(i.materialIndex);
    RasterFetch rf;
    MaterialInputs mi = EvalMaterialChannels<RasterFetch>(m, i.uv0, i.uv1, rf);
    return PbrShadeSurface(mi, m, i, frontFacing, fragCoord);
}
