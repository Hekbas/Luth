#include "luthpch.h"
#include "SlangCompiler.h"
#include "luth/core/diagnostics/Log.h"
#include "luth/resources/FileSystem.h"

#include <slang/slang.h>
#include <slang/slang-com-ptr.h>
#include <slang/slang-tag-version.h>
#include <fstream>
#include <mutex>

namespace Luth
{
    namespace
    {
        // invariant: a FRESH global session per compile, never shared. slang::IGlobalSession is not safe for
        // concurrent module loading; the asset pipeline compiles .slang on multiple threads, so a shared
        // session races (key-already-exists corruption + crashes). Per-compile isolation is the lock-free fix
        // (no worker-thread OS-sync blocking — see arch/fiber-system.md). createGlobalSession is thread-safe.
        bool CreateGlobal(Slang::ComPtr<slang::IGlobalSession>& out)
        {
            if (SLANG_FAILED(slang::createGlobalSession(out.writeRef())) || !out)
            {
                LH_CORE_ERROR("Slang: createGlobalSession failed — prebuilt DLLs missing or core module not found");
                return false;
            }
            static std::once_flag s_ready;
            std::call_once(s_ready, []() { LH_CORE_INFO("Slang in-process compiler ready ({})", SLANG_TAG_VERSION); });
            return true;
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

        ShaderStage FromSlangStage(SlangStage s)
        {
            switch (s)
            {
                case SLANG_STAGE_VERTEX:         return ShaderStage::Vertex;
                case SLANG_STAGE_FRAGMENT:       return ShaderStage::Fragment;
                case SLANG_STAGE_COMPUTE:        return ShaderStage::Compute;
                case SLANG_STAGE_RAY_GENERATION: return ShaderStage::Raygen;
                case SLANG_STAGE_MISS:           return ShaderStage::Miss;
                case SLANG_STAGE_CLOSEST_HIT:    return ShaderStage::ClosestHit;
                case SLANG_STAGE_ANY_HIT:        return ShaderStage::AnyHit;
                case SLANG_STAGE_INTERSECTION:   return ShaderStage::Intersection;
                case SLANG_STAGE_CALLABLE:       return ShaderStage::Callable;
                default:                         return ShaderStage::Unknown;
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
        bool MakeSession(slang::IGlobalSession* global, const fs::path& srcDir, Slang::ComPtr<slang::ISession>& out)
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

            // Import roots: the project's generated dir FIRST (its mat_graph_registry override shadows the
            // engine default), then the primary's own dir (loadModule finds it by name) + common/ for shared
            // modules, then registry/ LAST for the default mat_graph_registry. registry/ is deliberately NOT
            // common/: Slang resolves an import relative to the importing module's folder before the search
            // paths, so a default beside material_bindings_rt would always win over the project override.
            std::string genDir      = FileSystem::HasProject() ? FileSystem::ProjectPath("Library/Generated/shaders").string() : std::string();
            std::string srcDirStr   = srcDir.string();
            std::string commonDir   = FileSystem::EngineAssetsPath("shaders/common").string();
            std::string registryDir = FileSystem::EngineAssetsPath("shaders/registry").string();
            std::vector<const char*> searchPaths;
            if (!genDir.empty()) searchPaths.push_back(genDir.c_str());
            searchPaths.push_back(srcDirStr.c_str());
            searchPaths.push_back(commonDir.c_str());
            searchPaths.push_back(registryDir.c_str());

            slang::SessionDesc desc{};
            desc.targets = &target;
            desc.targetCount = 1;
            desc.defaultMatrixLayoutMode = SLANG_MATRIX_LAYOUT_COLUMN_MAJOR;
            desc.searchPaths = searchPaths.data();
            desc.searchPathCount = static_cast<SlangInt>(searchPaths.size());
            desc.compilerOptionEntries = opts;
            desc.compilerOptionEntryCount = static_cast<u32>(std::size(opts));

            return SLANG_SUCCEEDED(global->createSession(desc, out.writeRef())) && out;
        }

        // Shared front half of every compile: global session + source read + a fresh per-compile session
        // + module load. Returns the module (owned by outSession, which the caller must keep alive) or
        // nullptr on failure (already logged). Keeps the three public entry points from triplicating it.
        // outGlobal is created here and must outlive outSession (the caller keeps both alive).
        slang::IModule* PrepareModule(const fs::path& src,
                                      Slang::ComPtr<slang::IGlobalSession>& outGlobal,
                                      Slang::ComPtr<slang::ISession>& outSession)
        {
            if (!CreateGlobal(outGlobal)) return nullptr;

            std::string source = ReadFile(src);
            if (source.empty())
            {
                LH_CORE_ERROR("SlangCompiler: cannot read source '{}'", src.string());
                return nullptr;
            }
            if (!MakeSession(outGlobal.get(), src.parent_path(), outSession))
            {
                LH_CORE_ERROR("SlangCompiler: createSession failed for '{}'", src.string());
                return nullptr;
            }

            // Load the primary by name (file-based, like slangc); imports resolve from the search paths.
            Slang::ComPtr<slang::IBlob> diag;
            std::string moduleName = src.stem().string();
            slang::IModule* module = outSession->loadModule(moduleName.c_str(), diag.writeRef());
            if (!module) { LogDiag(diag, src); return nullptr; }
            return module;
        }
    }

    bool SlangCompiler::Available()
    {
        Slang::ComPtr<slang::IGlobalSession> g;
        return CreateGlobal(g);
    }

    std::vector<u32> SlangCompiler::Compile(const fs::path& sourcePath, const char* entryPoint, ShaderStage stage)
    {
        Slang::ComPtr<slang::IGlobalSession> global;   // must outlive `session` below
        Slang::ComPtr<slang::ISession> session;
        slang::IModule* module = PrepareModule(sourcePath, global, session);
        if (!module) return {};

        Slang::ComPtr<slang::IBlob> diag;
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

    SlangCompiler::CompileOutput SlangCompiler::CompileReflectStage(const fs::path& sourcePath, const char* entryPoint)
    {
        CompileOutput out;

        Slang::ComPtr<slang::IGlobalSession> global;   // must outlive `session` below
        Slang::ComPtr<slang::ISession> session;
        slang::IModule* module = PrepareModule(sourcePath, global, session);
        if (!module) return out;

        // No such entry → not a single-'main' shader (a multi-entry probe, or a module). Skip quietly so
        // the importer can treat it as "not a single-stage asset" rather than a compile error.
        Slang::ComPtr<slang::IEntryPoint> ep;
        if (SLANG_FAILED(module->findEntryPointByName(entryPoint, ep.writeRef())) || !ep)
            return out;

        slang::IComponentType* parts[] = { module, ep.get() };
        Slang::ComPtr<slang::IBlob> diag;
        Slang::ComPtr<slang::IComponentType> composed;
        if (SLANG_FAILED(session->createCompositeComponentType(parts, 2, composed.writeRef(), diag.writeRef())) || !composed)
        {
            LogDiag(diag, sourcePath);
            return out;
        }

        Slang::ComPtr<slang::IComponentType> linked;
        diag = nullptr;
        if (SLANG_FAILED(composed->link(linked.writeRef(), diag.writeRef())) || !linked)
        {
            LogDiag(diag, sourcePath);
            return out;
        }

        // Stage comes from the [shader("...")] attribute, surfaced through the linked program's reflection.
        diag = nullptr;
        if (slang::ProgramLayout* layout = linked->getLayout(0, diag.writeRef()))
            if (layout->getEntryPointCount() >= 1)
                out.stage = FromSlangStage(layout->getEntryPointByIndex(0)->getStage());

        Slang::ComPtr<slang::IBlob> spirv;
        diag = nullptr;
        if (SLANG_FAILED(linked->getEntryPointCode(0, 0, spirv.writeRef(), diag.writeRef())) || !spirv)
        {
            LogDiag(diag, sourcePath);
            return out;
        }
        out.spirv = BlobToWords(spirv);
        return out;
    }

    std::vector<std::vector<u32>> SlangCompiler::CompileModuleEntries(
        const fs::path& sourcePath, const std::vector<EntryReq>& entries)
    {
        std::vector<std::vector<u32>> result;
        if (entries.empty()) return result;

        Slang::ComPtr<slang::IGlobalSession> global;   // must outlive `session` below
        Slang::ComPtr<slang::ISession> session;
        slang::IModule* module = PrepareModule(sourcePath, global, session);
        if (!module) return result;

        Slang::ComPtr<slang::IBlob> diag;
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

    SlangCompiler::StructLayout SlangCompiler::ReflectStructLayout(const fs::path& sourcePath, const char* typeName)
    {
        StructLayout out;

        Slang::ComPtr<slang::IGlobalSession> global;   // must outlive `session` below
        Slang::ComPtr<slang::ISession> session;
        slang::IModule* module = PrepareModule(sourcePath, global, session);
        if (!module) return out;

        // A bare module's program layout exposes its public type decls — no entry point / link needed.
        Slang::ComPtr<slang::IBlob> diag;
        slang::ProgramLayout* layout = module->getLayout(0, diag.writeRef());
        if (!layout) { LogDiag(diag, sourcePath); return out; }

        slang::TypeReflection* type = layout->findTypeByName(typeName);
        if (!type)
        {
            LH_CORE_WARN("SlangCompiler: type '{}' not found in '{}'", typeName, sourcePath.filename().string());
            return out;
        }
        slang::TypeLayoutReflection* tl = layout->getTypeLayout(type, slang::LayoutRules::Default);
        if (!tl)
        {
            LH_CORE_WARN("SlangCompiler: no layout for '{}' in '{}'", typeName, sourcePath.filename().string());
            return out;
        }

        // Default ParameterCategory = Uniform → byte offsets/size matching the std140/std430 GPU view.
        out.size = tl->getSize();
        unsigned n = tl->getFieldCount();
        out.fields.reserve(n);
        for (unsigned i = 0; i < n; ++i)
        {
            if (slang::VariableLayoutReflection* f = tl->getFieldByIndex(i))
            {
                const char* fname = f->getName();
                out.fields.push_back({ fname ? fname : "", f->getOffset() });
            }
        }
        out.ok = true;
        return out;
    }
}
