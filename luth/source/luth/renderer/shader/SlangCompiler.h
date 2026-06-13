#pragma once

#include "luth/core/types/LuthTypes.h"
#include "luth/renderer/shader/Shader.h"
#include <filesystem>
#include <vector>

namespace Luth
{
    // In-process Slang -> SPIR-V compiler, coexisting with the libshaderc ShaderCompiler (no removals).
    // Owns a lazily-created process-global slang::IGlobalSession — nothing loads slang-compiler.dll until
    // the first call, so the slang-spike stays zero-cost while its toggle is off. Each Compile spins a
    // fresh per-compile ISession (column-major matrices + precise fp to keep the GLSL A/B comparable;
    // debug info on for RenderDoc source correlation). Synchronous on the calling fiber, mirroring
    // ShaderCompiler — compiles are serialized at subsystem init, not a frame hot path. Phase-0 gate #156.
    class SlangCompiler
    {
    public:
        // Compile one entry point of a .slang file to SPIR-V. entryPoint = the function name; when stage
        // is Unknown the [shader("...")] attribute on that function supplies it, otherwise stage is used
        // to resolve attribute-less entries. Returns empty on failure (diagnostics logged).
        static std::vector<u32> Compile(const std::filesystem::path& sourcePath,
                                        const char* entryPoint = "main",
                                        ShaderStage stage = ShaderStage::Unknown);

        // Link-time-specialization probe (#156 item 6 / slang#9578): compose ALL the named entry points
        // into ONE linked program, then emit SPIR-V per entry — the "one body, link-specialized per
        // stage" shape. Returns one blob per request, in order; a failed entry yields an empty blob.
        struct EntryReq { const char* name; ShaderStage stage; };
        static std::vector<std::vector<u32>> CompileModuleEntries(
            const std::filesystem::path& sourcePath, const std::vector<EntryReq>& entries);

        // True once the global session creates successfully (prebuilt DLLs loaded + core module found).
        // First call pays the slang-compiler.dll load; safe to probe behind the spike toggle.
        static bool Available();
    };
}
