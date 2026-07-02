#pragma once

#include "luth/core/UUID.h"

namespace Luth
{
    class Material;

    // Lowers a Material's node graph to a generated Slang fragment shader (a channel-routing EvalGraph<F>
    // module + a thin pbr_graph consumer), compiles it via SlangCompiler, registers it in ShaderLibrary,
    // and stores the resulting shader UUID on the material (SetGraphShaderUUID). Returns that UUID, or
    // Invalid on an empty graph / no project / compile failure; the material then renders with the stock
    // pbr fragment. Channel-routing nodes are derivative-free, so the emitted body is two-tier-safe by
    // construction (the RT tier reuses the same EvalGraph in a later effort). see arch/rendering-pipeline.md
    namespace MaterialGraphCodegen
    {
        UUID GenerateAndCompile(Material& material);

        // Recompute the material's cached graph constants from node values; no compile, no variant change.
        // The value-edit path: a Const/Remap value change is data, lowered into gMatParams the next frame.
        void RefreshParams(Material& material);
    }
}
