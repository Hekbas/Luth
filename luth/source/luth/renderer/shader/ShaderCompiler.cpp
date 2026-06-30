#include "luthpch.h"
#include "ShaderCompiler.h"
#include "SlangCompiler.h"
#include "luth/core/diagnostics/Log.h"

// Slang is the engine's only shader language. ShaderCompiler is a thin dispatch over SlangCompiler;
// the stage lives in the .slang [shader("...")] attribute and is recovered via reflection. see arch/asset-pipeline.md
namespace Luth
{
    std::vector<u32> ShaderCompiler::Compile(const std::filesystem::path& sourcePath, bool /*optimize*/)
    {
        LH_PROFILE_FUNCTION();
        if (sourcePath.extension() == ".slang")
            return SlangCompiler::Compile(sourcePath, "main");

        LH_LOG(Shaders, error, "ShaderCompiler: only .slang shaders are supported ('{}')", sourcePath.string());
        return {};
    }

    ShaderCompiler::StagedSpirv ShaderCompiler::CompileStaged(const std::filesystem::path& sourcePath, bool /*optimize*/)
    {
        LH_PROFILE_FUNCTION();
        if (sourcePath.extension() == ".slang")
        {
            SlangCompiler::CompileOutput r = SlangCompiler::CompileReflectStage(sourcePath, "main");
            return { std::move(r.spirv), r.stage };
        }

        LH_LOG(Shaders, error, "ShaderCompiler: only .slang shaders are supported ('{}')", sourcePath.string());
        return {};
    }
}
