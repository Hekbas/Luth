#include "luthpch.h"
#include "ShaderCompiler.h"
#include "luth/core/Log.h"
#include <shaderc/shaderc.hpp>
#include <fstream>

#ifdef _MSC_VER
    #pragma comment(lib, "shaderc_shared.lib")
#endif

namespace Luth
{
    ShaderStage ShaderCompiler::InferStage(const fs::path& path)
    {
        std::string ext = path.extension().string();
        if (ext == ".vert") return ShaderStage::Vertex;
        if (ext == ".frag") return ShaderStage::Fragment;
        if (ext == ".comp") return ShaderStage::Compute;
        return ShaderStage::Unknown;
    }

    static shaderc_shader_kind ToShadercKind(ShaderStage stage)
    {
        switch (stage)
        {
            case ShaderStage::Vertex:   return shaderc_glsl_vertex_shader;
            case ShaderStage::Fragment: return shaderc_glsl_fragment_shader;
            case ShaderStage::Compute:  return shaderc_glsl_compute_shader;
            default:                    return shaderc_glsl_infer_from_source;
        }
    }

    std::vector<u32> ShaderCompiler::Compile(const fs::path& sourcePath, bool optimize)
    {
        ShaderStage stage = InferStage(sourcePath);
        if (stage == ShaderStage::Unknown)
        {
            LH_CORE_ERROR("ShaderCompiler: unsupported shader extension for '{}'", sourcePath.string());
            return {};
        }

        // Read source
        std::ifstream file(sourcePath, std::ios::ate | std::ios::binary);
        if (!file.is_open()) {
            LH_CORE_ERROR("Failed to open shader source: {0}", sourcePath.string());
            return {};
        }

        size_t fileSize = (size_t)file.tellg();
        std::vector<char> buffer(fileSize);
        file.seekg(0);
        file.read(buffer.data(), fileSize);
        file.close();

        std::string sourceStr(buffer.begin(), buffer.end());

        // Compile
        shaderc::Compiler compiler;
        shaderc::CompileOptions options;
        options.SetTargetEnvironment(shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_3);
        if (optimize) options.SetOptimizationLevel(shaderc_optimization_level_performance);

        shaderc::SpvCompilationResult module = compiler.CompileGlslToSpv(
            sourceStr,
            ToShadercKind(stage),
            sourcePath.filename().string().c_str(),
            options
        );

        if (module.GetCompilationStatus() != shaderc_compilation_status_success) {
            LH_CORE_ERROR("Shader Compilation Error ({0}):\n{1}", sourcePath.string(), module.GetErrorMessage());
            return {};
        }

        return { module.cbegin(), module.cend() };
    }
}
