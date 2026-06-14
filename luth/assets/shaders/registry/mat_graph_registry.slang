// Default node-graph variant registry — the fallback the RT megakernels compile against when no project
// override exists. MUST NOT live in common/: Slang resolves an import relative to the importing module's
// folder first, so a copy beside material_bindings_rt (which imports it) would always shadow the project's
// generated override on the search path. Kept in its own dir, added LAST to the search path, so the
// project's Library/Generated copy (added first) wins whenever it exists. see arch/rendering-pipeline.md
import material;

public MaterialInputs EvalGraphVariant<F : ITexFetch>(uint variant, GPUMaterialData m, float2 uv0, float2 uv1, F fetch)
{
    return EvalMaterialChannels<F>(m, uv0, uv1, fetch);
}
