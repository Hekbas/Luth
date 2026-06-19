#include "luthpch.h"
#include "ShaderCompiler.h"
#include "SlangCompiler.h"
#include "luth/core/diagnostics/Log.h"
#include "luth/resources/FileSystem.h"
#include <shaderc/shaderc.hpp>
#include <fstream>
#include <cstring>

#ifdef _MSC_VER
    #pragma comment(lib, "shaderc_shared.lib")
#endif

namespace Luth
{
    namespace
    {
        // Resolves #include directives. Relative includes (#include "x.glsl") look in the requesting
        // source's directory; angled includes (#include <x.glsl>) and unresolved relative paths fall
        // back to the engine shader root (luth/assets/shaders/).
        class LuthIncluder : public shaderc::CompileOptions::IncluderInterface
        {
        public:
            explicit LuthIncluder(fs::path shaderRoot) : m_Root(std::move(shaderRoot)) {}

            shaderc_include_result* GetInclude(const char* requested,
                                               shaderc_include_type type,
                                               const char* requesting,
                                               size_t /*depth*/) override
            {
                fs::path resolved;
                if (type == shaderc_include_type_relative && requesting && *requesting)
                {
                    resolved = fs::path(requesting).parent_path() / requested;
                    if (!fs::exists(resolved))
                        resolved = m_Root / requested;
                }
                else
                {
                    resolved = m_Root / requested;
                }

                auto* result  = new shaderc_include_result{};
                auto* payload = new Payload{};

                if (!fs::exists(resolved))
                {
                    payload->content = "// Luth includer: file not found: ";
                    payload->content += requested;
                    payload->name    = std::string(requested);
                }
                else
                {
                    std::ifstream f(resolved, std::ios::binary);
                    std::ostringstream ss; ss << f.rdbuf();
                    payload->content = ss.str();
                    payload->name    = resolved.string();
                }

                result->source_name        = payload->name.c_str();
                result->source_name_length = payload->name.size();
                result->content            = payload->content.c_str();
                result->content_length     = payload->content.size();
                result->user_data          = payload;
                return result;
            }

            void ReleaseInclude(shaderc_include_result* result) override
            {
                if (!result) return;
                delete static_cast<Payload*>(result->user_data);
                delete result;
            }

        private:
            struct Payload { std::string name; std::string content; };
            fs::path m_Root;
        };
    }

    ShaderStage ShaderCompiler::InferStage(const fs::path& path)
    {
        std::string ext = path.extension().string();
        if (ext == ".vert")  return ShaderStage::Vertex;
        if (ext == ".frag")  return ShaderStage::Fragment;
        if (ext == ".comp")  return ShaderStage::Compute;
        if (ext == ".rgen")  return ShaderStage::Raygen;
        if (ext == ".rmiss") return ShaderStage::Miss;
        if (ext == ".rchit") return ShaderStage::ClosestHit;
        if (ext == ".rahit") return ShaderStage::AnyHit;
        if (ext == ".rint")  return ShaderStage::Intersection;
        if (ext == ".rcall") return ShaderStage::Callable;
        return ShaderStage::Unknown;
    }

    static shaderc_shader_kind ToShadercKind(ShaderStage stage)
    {
        switch (stage)
        {
            case ShaderStage::Vertex:       return shaderc_glsl_vertex_shader;
            case ShaderStage::Fragment:     return shaderc_glsl_fragment_shader;
            case ShaderStage::Compute:      return shaderc_glsl_compute_shader;
            case ShaderStage::Raygen:       return shaderc_glsl_raygen_shader;
            case ShaderStage::Miss:         return shaderc_glsl_miss_shader;
            case ShaderStage::ClosestHit:   return shaderc_glsl_closesthit_shader;
            case ShaderStage::AnyHit:       return shaderc_glsl_anyhit_shader;
            case ShaderStage::Intersection: return shaderc_glsl_intersection_shader;
            case ShaderStage::Callable:     return shaderc_glsl_callable_shader;
            default:                        return shaderc_glsl_infer_from_source;
        }
    }

    std::vector<u32> ShaderCompiler::Compile(const fs::path& sourcePath, bool optimize)
    {
        LH_PROFILE_FUNCTION();
        // .slang dispatches to the in-process Slang backend (coexists with libshaderc, no removals); the
        // entry's [shader("...")] attribute supplies the stage. see SlangCompiler / spike #156
        if (sourcePath.extension() == ".slang")
            return SlangCompiler::Compile(sourcePath, "main");

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
        options.SetIncluder(std::make_unique<LuthIncluder>(FileSystem::EngineAssetsPath("shaders")));

        shaderc::SpvCompilationResult module = compiler.CompileGlslToSpv(
            sourceStr,
            ToShadercKind(stage),
            sourcePath.string().c_str(),  // full path so includer can resolve relative includes
            options
        );

        if (module.GetCompilationStatus() != shaderc_compilation_status_success) {
            LH_CORE_ERROR("Shader Compilation Error ({0}):\n{1}", sourcePath.string(), module.GetErrorMessage());
            return {};
        }

        return { module.cbegin(), module.cend() };
    }

    ShaderCompiler::StagedSpirv ShaderCompiler::CompileStaged(const fs::path& sourcePath, bool optimize)
    {
        LH_PROFILE_FUNCTION();
        // .slang stage lives in the source attribute, not the extension — recover it via reflection. GLSL
        // keeps the cheap extension inference. Either way the importer gets {spirv, stage} from one call.
        if (sourcePath.extension() == ".slang")
        {
            SlangCompiler::CompileOutput r = SlangCompiler::CompileReflectStage(sourcePath, "main");
            return { std::move(r.spirv), r.stage };
        }
        return { Compile(sourcePath, optimize), InferStage(sourcePath) };
    }
}
