#pragma once

#include "luth/core/types/LuthTypes.h"

#include <string>
#include <vector>

namespace Luth
{
    class RenderPipeline;

    // Bindless-SPIR-V codegen regression gate. Deterministically scans the compiled SPIR-V of a production
    // rayQuery shader (restir_gi_initial.slang) for the NonUniform decorations + the bindless / rayQuery /
    // BDA capabilities that slang#10525-class regressions drop or misplace. Runs once at init and on that
    // shader's hot-reload; no GPU, no frame. The verdict surfaces through SlangParitySettings, which
    // RenderPanel reads read-only.
    class SlangParityGuard
    {
    public:
        void Init(RenderPipeline& pipeline);
        void Shutdown();

        // Re-run the gate when the watched production shader hot-reloads. True iff it was the watched shader.
        bool OnShaderReloaded(const std::string& name, const std::vector<u32>& spv);

    private:
        void RunGate();          // pull the watched shader's SPIR-V from the ShaderLibrary cache -> scan
        void CheckSlangSpirv();  // scan m_GateSpv, write the verdict into SlangParitySettings

        RenderPipeline*  m_Pipeline = nullptr;
        std::vector<u32> m_GateSpv;
    };
}
