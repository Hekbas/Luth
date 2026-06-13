#include "luthpch.h"
#include "SlangCompiler.h"
#include "luth/core/diagnostics/Log.h"
#include "luth/resources/FileSystem.h"

#include <slang/slang.h>
#include <slang/slang-com-ptr.h>
#include <slang/slang-tag-version.h>
#include <fstream>

namespace Luth
{
    namespace
    {
        // Lazily-created, process-global. The magic-static init runs once (thread-safe); the session
        // object itself is shared, which is fine because compiles are serialized at subsystem init (the
        // same single-threaded-compile contract ShaderCompiler relies on). Returns nullptr if the
        // prebuilt DLLs failed to load or the core module wasn't found.
        slang::IGlobalSession* GetGlobalSession()
        {
            static Slang::ComPtr<slang::IGlobalSession> s_Global = []() {
                Slang::ComPtr<slang::IGlobalSession> g;
                if (SLANG_FAILED(slang::createGlobalSession(g.writeRef())) || !g)
                {
                    LH_CORE_ERROR("Slang: createGlobalSession failed — prebuilt DLLs missing or core module not found");
                    return Slang::ComPtr<slang::IGlobalSession>();
                }
                LH_CORE_INFO("Slang in-process compiler ready ({})", SLANG_TAG_VERSION);
                return g;
            }();
            return s_Global.get();
        }

        SlangStage ToSlangStage(ShaderStage s)
        {
            switch (s)
            {
                case ShaderStage::Vertex:       return SLANG_STAGE_VERTEX;
                case ShaderStage::Fragment:     return SLANG_STAGE_FRAGMENT;
                case ShaderStage::Compute:      return SLANG_STAGE_COMPUTE;
                case ShaderStage::Raygen:       return SLANG_STAGE_RAY_GENERATION;
                case ShaderStage::Miss:         return SLANG_STAGE_MISS;
                case ShaderStage::ClosestHit:   return SLANG_STAGE_CLOSEST_HIT;
                case ShaderStage::AnyHit:       return SLANG_STAGE_ANY_HIT;
                case ShaderStage::Intersection: return SLANG_STAGE_INTERSECTION;
                case ShaderStage::Callable:     return SLANG_STAGE_CALLABLE;
                default:                        return SLANG_STAGE_NONE;
            }
        }

        void LogDiag(slang::IBlob* d, const fs::path& ctx)
        {
            if (d && d->getBufferSize() > 0)
                LH_CORE_ERROR("Slang ({}):\n{}", ctx.filename().string(),
                              static_cast<const char*>(d->getBufferPointer()));
        }

        std::vector<u32> BlobToWords(slang::IBlob* b)
        {
            const u32* words = static_cast<const u32*>(b->getBufferPointer());
            size_t n = b->getBufferSize() / sizeof(u32);
            return { words, words + n };
        }

        std::string ReadFile(const fs::path& p)
        {
            std::ifstream f(p, std::ios::ate | std::ios::binary);
            if (!f.is_open()) return {};
            size_t sz = static_cast<size_t>(f.tellg());
            std::string s(sz, '\0');
            f.seekg(0);
            f.read(s.data(), sz);
            return s;
        }

        // One SPIR-V target + the two load-bearing parity knobs (column-major matrices match our std430
        // Mat4 push constants; precise fp blocks FMA reassociation so the GLSL A/B closes to a few ULP).
        // Debug info + direct-SPIRV backend (kDefaultTargetFlags) are the codegen path under test.
        bool MakeSession(slang::IGlobalSession* global, Slang::ComPtr<slang::ISession>& out)
        {
            slang::TargetDesc target{};
            target.format = SLANG_SPIRV;
            target.profile = global->findProfile("spirv_1_5");
            target.flags = kDefaultTargetFlags;             // SLANG_TARGET_FLAG_GENERATE_SPIRV_DIRECTLY
            target.floatingPointMode = SLANG_FLOATING_POINT_MODE_PRECISE;
            if (target.profile == SLANG_PROFILE_UNKNOWN)
                LH_CORE_WARN("Slang: profile 'spirv_1_5' unknown on this build — emitting at target default");

            slang::CompilerOptionEntry opts[] = {
                { slang::CompilerOptionName::DebugInformation,
                  { slang::CompilerOptionValueKind::Int, SLANG_DEBUG_INFO_LEVEL_STANDARD, 0, nullptr, nullptr } },
                { slang::CompilerOptionName::Optimization,
                  { slang::CompilerOptionValueKind::Int, SLANG_OPTIMIZATION_LEVEL_NONE, 0, nullptr, nullptr } },
                { slang::CompilerOptionName::VulkanUseEntryPointName,
                  { slang::CompilerOptionValueKind::Int, 1, 0, nullptr, nullptr } },
                // 41012 = "profile auto-upgraded to include the caps the entry point needs" — always
                // benign for our hot-path shaders (rayQuery + the texture caps beyond bare spirv_1_5);
                // spirv-val is the real correctness gate. Suppress so it can't read as an engine error.
                { slang::CompilerOptionName::DisableWarning,
                  { slang::CompilerOptionValueKind::String, 0, 0, "41012", nullptr } },
            };

            std::string searchDir = FileSystem::EngineAssetsPath("shaders").string();
            const char* searchPaths[] = { searchDir.c_str() };

            slang::SessionDesc desc{};
            desc.targets = &target;
            desc.targetCount = 1;
            desc.defaultMatrixLayoutMode = SLANG_MATRIX_LAYOUT_COLUMN_MAJOR;
            desc.searchPaths = searchPaths;
            desc.searchPathCount = 1;
            desc.compilerOptionEntries = opts;
            desc.compilerOptionEntryCount = static_cast<u32>(std::size(opts));

            return SLANG_SUCCEEDED(global->createSession(desc, out.writeRef())) && out;
        }
    }

    bool SlangCompiler::Available()
    {
        return GetGlobalSession() != nullptr;
    }

    std::vector<u32> SlangCompiler::Compile(const fs::path& sourcePath, const char* entryPoint, ShaderStage stage)
    {
        slang::IGlobalSession* global = GetGlobalSession();
        if (!global) return {};

        std::string source = ReadFile(sourcePath);
        if (source.empty())
        {
            LH_CORE_ERROR("SlangCompiler: cannot read source '{}'", sourcePath.string());
            return {};
        }

        Slang::ComPtr<slang::ISession> session;
        if (!MakeSession(global, session))
        {
            LH_CORE_ERROR("SlangCompiler: createSession failed for '{}'", sourcePath.string());
            return {};
        }

        Slang::ComPtr<slang::IBlob> diag;
        std::string moduleName = sourcePath.stem().string();
        slang::IModule* module = session->loadModuleFromSourceString(
            moduleName.c_str(), sourcePath.string().c_str(), source.c_str(), diag.writeRef());
        if (!module) { LogDiag(diag, sourcePath); return {}; }   // LogDiag only on failure (the diag is the error)

        // Entry point: with a known stage use findAndCheckEntryPoint (works without a [shader] attr);
        // otherwise the function's own [shader("...")] attribute supplies the stage.
        Slang::ComPtr<slang::IEntryPoint> ep;
        diag = nullptr;
        if (stage == ShaderStage::Unknown)
        {
            if (SLANG_FAILED(module->findEntryPointByName(entryPoint, ep.writeRef())) || !ep)
            {
                LH_CORE_ERROR("SlangCompiler: entry '{}' not found in '{}' (needs a [shader(...)] attribute)",
                              entryPoint, sourcePath.string());
                return {};
            }
        }
        else if (SLANG_FAILED(module->findAndCheckEntryPoint(entryPoint, ToSlangStage(stage), ep.writeRef(), diag.writeRef())) || !ep)
        {
            LogDiag(diag, sourcePath);
            LH_CORE_ERROR("SlangCompiler: entry '{}' not found in '{}'", entryPoint, sourcePath.string());
            return {};
        }

        slang::IComponentType* parts[] = { module, ep.get() };
        Slang::ComPtr<slang::IComponentType> composed;
        diag = nullptr;
        if (SLANG_FAILED(session->createCompositeComponentType(parts, 2, composed.writeRef(), diag.writeRef())) || !composed)
        {
            LogDiag(diag, sourcePath);
            return {};
        }

        Slang::ComPtr<slang::IComponentType> linked;
        diag = nullptr;
        if (SLANG_FAILED(composed->link(linked.writeRef(), diag.writeRef())) || !linked)
        {
            LogDiag(diag, sourcePath);
            return {};
        }

        Slang::ComPtr<slang::IBlob> spirv;
        diag = nullptr;
        if (SLANG_FAILED(linked->getEntryPointCode(0, 0, spirv.writeRef(), diag.writeRef())) || !spirv)
        {
            LogDiag(diag, sourcePath);
            return {};
        }
        return BlobToWords(spirv);
    }

    std::vector<std::vector<u32>> SlangCompiler::CompileModuleEntries(
        const fs::path& sourcePath, const std::vector<EntryReq>& entries)
    {
        std::vector<std::vector<u32>> result;
        slang::IGlobalSession* global = GetGlobalSession();
        if (!global || entries.empty()) return result;

        std::string source = ReadFile(sourcePath);
        if (source.empty())
        {
            LH_CORE_ERROR("SlangCompiler: cannot read source '{}'", sourcePath.string());
            return result;
        }

        Slang::ComPtr<slang::ISession> session;
        if (!MakeSession(global, session)) return result;

        Slang::ComPtr<slang::IBlob> diag;
        std::string moduleName = sourcePath.stem().string();
        slang::IModule* module = session->loadModuleFromSourceString(
            moduleName.c_str(), sourcePath.string().c_str(), source.c_str(), diag.writeRef());
        if (!module) { LogDiag(diag, sourcePath); return result; }   // LogDiag only on failure

        // Compose EVERY entry into one program so link-time specialization spans the stages — the exact
        // shape Phase 2's shared IMaterial eval needs. getEntryPointCode below emits each stage from it.
        std::vector<Slang::ComPtr<slang::IEntryPoint>> eps;
        std::vector<slang::IComponentType*> parts;
        parts.push_back(module);
        for (const auto& req : entries)
        {
            Slang::ComPtr<slang::IEntryPoint> ep;
            diag = nullptr;
            if (SLANG_FAILED(module->findAndCheckEntryPoint(req.name, ToSlangStage(req.stage), ep.writeRef(), diag.writeRef())) || !ep)
            {
                LogDiag(diag, sourcePath);
                LH_CORE_ERROR("SlangCompiler: link-spec entry '{}' not found in '{}'", req.name, sourcePath.string());
                return result;
            }
            parts.push_back(ep.get());
            eps.push_back(ep);
        }

        Slang::ComPtr<slang::IComponentType> composed;
        diag = nullptr;
        if (SLANG_FAILED(session->createCompositeComponentType(parts.data(), (SlangInt)parts.size(), composed.writeRef(), diag.writeRef())) || !composed)
        {
            LogDiag(diag, sourcePath);
            return result;
        }

        Slang::ComPtr<slang::IComponentType> linked;
        diag = nullptr;
        if (SLANG_FAILED(composed->link(linked.writeRef(), diag.writeRef())) || !linked)
        {
            LogDiag(diag, sourcePath);
            return result;
        }

        // Emit one blob per requested entry (entry index i == entries[i]; the module contributes none).
        // A stage that mis-emits leaves an empty blob — that IS the slang#9578 signal the caller checks.
        for (size_t i = 0; i < entries.size(); ++i)
        {
            Slang::ComPtr<slang::IBlob> spirv;
            diag = nullptr;
            if (SLANG_FAILED(linked->getEntryPointCode((SlangInt)i, 0, spirv.writeRef(), diag.writeRef())) || !spirv)
            {
                LogDiag(diag, sourcePath);
                result.push_back({});
            }
            else
            {
                result.push_back(BlobToWords(spirv));
            }
        }
        return result;
    }
}
