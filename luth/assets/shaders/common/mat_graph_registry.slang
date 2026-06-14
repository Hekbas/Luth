// Default node-graph variant registry. The RT megakernels share one kernel over all materials, so they
// dispatch the per-material graph decode through EvalGraphVariant rather than binding a per-material shader
// (the raster path's model). This engine default has no variants — every variant resolves to the stock
// channel decode. MaterialGraphCodegen writes a project copy under Library/Generated that shadows this on
// the Slang search path, adding one switch arm per graph material. see arch/rendering-pipeline.md
import material;

public MaterialInputs EvalGraphVariant<F : ITexFetch>(uint variant, GPUMaterialData m, float2 uv0, float2 uv1, F fetch)
{
    return EvalMaterialChannels<F>(m, uv0, uv1, fetch);
}
