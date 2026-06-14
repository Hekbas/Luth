#include "luthpch.h"
#include "luth/renderer/material/MaterialGraphCodegen.h"
#include "luth/renderer/material/Material.h"
#include "luth/renderer/shader/SlangCompiler.h"
#include "luth/renderer/shader/Shader.h"
#include "luth/renderer/shader/ShaderLibrary.h"
#include "luth/resources/FileSystem.h"
#include "luth/core/diagnostics/Log.h"

#include <fstream>
#include <sstream>
#include <iomanip>
#include <cmath>
#include <unordered_map>
#include <unordered_set>

namespace Luth
{
    namespace
    {
        // Slang identifiers / module names can't contain '-'; UUID::ToString emits them.
        std::string SanitizeIdent(std::string s)
        {
            for (char& c : s) if (c == '-') c = '_';
            return s;
        }

        // Float -> Slang literal, always carrying a decimal/exponent so it never tokenizes as an int.
        std::string F(f32 v)
        {
            std::ostringstream o; o << std::setprecision(9) << v;
            std::string s = o.str();
            if (s.find_first_of(".eEni") == std::string::npos) s += ".0";   // 'n'/'i' guard inf/nan spellings
            return s;
        }

        // fetch.Sample(...) for a TextureSample node's map slot. Maps carrying a per-map UV-index bit follow
        // it; the rest sample UV0 — mirrors EvalMaterialChannels in material.slang.
        std::string TexSampleExpr(u32 tex)
        {
            switch (static_cast<MapType>(tex))
            {
                case MapType::Diffuse:   return "fetch.Sample(m.diffuseIndex, SelectUV(m.flags, UV_SHIFT_DIFFUSE, uv0, uv1))";
                case MapType::Normal:    return "fetch.Sample(m.normalIndex, SelectUV(m.flags, UV_SHIFT_NORMAL, uv0, uv1))";
                case MapType::Metalness:
                case MapType::Roughness: return "fetch.Sample(m.metalRoughIndex, SelectUV(m.flags, UV_SHIFT_METALROUGH, uv0, uv1))";
                case MapType::Occlusion: return "fetch.Sample(m.occlusionIndex, SelectUV(m.flags, UV_SHIFT_OCCLUSION, uv0, uv1))";
                case MapType::Emissive:  return "fetch.Sample(m.emissiveIndex, uv0)";
                case MapType::Alpha:     return "fetch.Sample(m.alphaIndex, uv0)";
                case MapType::Specular:  return "fetch.Sample(m.specularIndex, uv0)";
                case MapType::Thickness: return "fetch.Sample(m.thicknessIndex, uv0)";
                default:                 return "float4(0.0)";
            }
        }

        // Lowers a MaterialGraph to the EvalGraph<F> module body. Every node becomes one float4 SSA local
        // (scalars broadcast — uniform typing, no inference); the Output node overrides only its connected
        // channels, so an empty/partial graph degrades cleanly to the stock EvalMaterialChannels baseline.
        struct Lowerer
        {
            const MaterialGraph& g;
            std::unordered_map<u32, const MatNode*> byId;
            std::ostringstream body;

            explicit Lowerer(const MaterialGraph& graph) : g(graph)
            {
                for (const auto& n : g.nodes) byId[n.id] = &n;
            }

            const MatLink* Incoming(u32 nodeId, u8 slot) const
            {
                for (const auto& l : g.links)
                    if (l.toNode == nodeId && l.toSlot == slot) return &l;
                return nullptr;
            }

            // float4 expression reading a producer node's output slot (Split exposes channels, else n<id>).
            std::string SourceExpr(const MatNode& prod, u8 fromSlot) const
            {
                if (prod.type == MatNodeType::Split)
                {
                    const char sw[4] = { 'x', 'y', 'z', 'w' };
                    return "float4(n" + std::to_string(prod.id) + "." + sw[fromSlot & 3] + ")";
                }
                return "n" + std::to_string(prod.id);
            }

            // float4 feeding a consumer node's input slot — linked source, else an identity-aware default.
            std::string Input(const MatNode& n, u8 slot) const
            {
                if (const MatLink* l = Incoming(n.id, slot))
                    if (auto it = byId.find(l->fromNode); it != byId.end())
                        return SourceExpr(*it->second, l->fromSlot);

                switch (n.type)
                {
                    case MatNodeType::Multiply: return "float4(1.0)";
                    case MatNodeType::Lerp:     return slot == 2 ? "float4(0.5)" : "float4(0.0)";
                    default:                    return "float4(0.0)";
                }
            }

            std::string NodeRhs(const MatNode& n) const
            {
                switch (n.type)
                {
                    case MatNodeType::ConstFloat:    return "float4(" + F(n.value.x) + ")";
                    case MatNodeType::ConstColor:    return "float4(" + F(n.value.x) + ", " + F(n.value.y) + ", "
                                                                      + F(n.value.z) + ", " + F(n.value.w) + ")";
                    case MatNodeType::TextureSample: return TexSampleExpr(n.tex);
                    case MatNodeType::Multiply:      return "(" + Input(n, 0) + " * " + Input(n, 1) + ")";
                    case MatNodeType::Add:           return "(" + Input(n, 0) + " + " + Input(n, 1) + ")";
                    case MatNodeType::Lerp:          return "lerp(" + Input(n, 0) + ", " + Input(n, 1) + ", " + Input(n, 2) + ")";
                    case MatNodeType::Remap:
                    {
                        // value = (inMin, inMax, outMin, outMax) precomputed to an affine x*scale + bias.
                        f32 denom = n.value.y - n.value.x;
                        f32 scale = (std::abs(denom) < 1e-6f) ? 0.0f : (n.value.w - n.value.z) / denom;
                        f32 bias  = n.value.z - n.value.x * scale;
                        return "(" + Input(n, 0) + " * " + F(scale) + " + float4(" + F(bias) + "))";
                    }
                    case MatNodeType::Split:         return Input(n, 0);   // pass-through; channels read via SourceExpr
                    default:                         return "float4(0.0)";
                }
            }

            u8 InputCount(MatNodeType t) const
            {
                if (t == MatNodeType::Lerp) return 3;
                if (t == MatNodeType::Multiply || t == MatNodeType::Add) return 2;
                return 1;   // Remap / Split (Const / TextureSample have none — slot 0 simply stays unlinked)
            }

            // DFS post-order: emit a node's dependencies, then its SSA local. Guards re-emission + cycles
            // (the editor forbids cycles via AllowedLink; this stays defensive against a hand-edited .mat).
            void Emit(u32 nodeId, std::unordered_set<u32>& done, std::unordered_set<u32>& active)
            {
                if (done.count(nodeId) || active.count(nodeId)) return;
                auto it = byId.find(nodeId);
                if (it == byId.end() || it->second->type == MatNodeType::Output) return;

                const MatNode& n = *it->second;
                active.insert(nodeId);
                const u8 inCount = InputCount(n.type);
                for (u8 s = 0; s < inCount; ++s)
                    if (const MatLink* l = Incoming(nodeId, s)) Emit(l->fromNode, done, active);
                active.erase(nodeId);

                done.insert(nodeId);
                body << "    float4 n" << nodeId << " = " << NodeRhs(n) << ";\n";
            }

            std::string Run()
            {
                const MatNode* out = nullptr;
                for (const auto& n : g.nodes) if (n.type == MatNodeType::Output) { out = &n; break; }

                std::ostringstream ss;
                ss << "import material;\n\n";
                ss << "public MaterialInputs EvalGraph<F : ITexFetch>(GPUMaterialData m, float2 uv0, float2 uv1, F fetch)\n{\n";
                ss << "    MaterialInputs mi = EvalMaterialChannels<F>(m, uv0, uv1, fetch);\n";

                if (out)
                {
                    std::unordered_set<u32> done, active;
                    for (u8 s = 0; s < 6; ++s)
                        if (const MatLink* l = Incoming(out->id, s)) Emit(l->fromNode, done, active);
                    ss << body.str();

                    // Source expr for an Output channel, or empty if unconnected / dangling.
                    auto OutSrc = [&](u8 slot) -> std::string {
                        const MatLink* l = Incoming(out->id, slot);
                        if (!l) return {};
                        auto it = byId.find(l->fromNode);
                        return it == byId.end() ? std::string{} : SourceExpr(*it->second, l->fromSlot);
                    };

                    std::string s;
                    if (s = OutSrc(0); !s.empty()) ss << "    mi.baseColor = " << s << ";\n";
                    if (s = OutSrc(1); !s.empty()) ss << "    mi.metallic  = " << s << ".x;\n";
                    if (s = OutSrc(2); !s.empty()) ss << "    mi.roughness = clamp(" << s << ".x, 0.04, 1.0);\n";
                    if (s = OutSrc(3); !s.empty()) ss << "    mi.normal    = " << s << ".xyz;\n";
                    if (s = OutSrc(4); !s.empty()) ss << "    mi.occlusion = " << s << ".x;\n";
                    if (s = OutSrc(5); !s.empty()) ss << "    mi.emissive  = " << s << ".rgb;\n";
                }
                ss << "    return mi;\n}\n";
                return ss.str();
            }
        };

        // The per-material consumer: pbr.slang with the decode swapped to the graph's EvalGraph.
        std::string EmitConsumer(const std::string& moduleName)
        {
            std::ostringstream ss;
            ss << "import globals;\n";
            ss << "import material;\n";
            ss << "import material_bindings_raster;\n";
            ss << "import pbr_shade;\n";
            ss << "import " << moduleName << ";\n\n";
            ss << "[shader(\"fragment\")]\n";
            ss << "FOut main(VIn i, bool frontFacing : SV_IsFrontFace, float4 fragCoord : SV_Position)\n{\n";
            ss << "    GPUMaterialData m = GetMaterial(i.materialIndex);\n";
            ss << "    RasterFetch rf;\n";
            ss << "    MaterialInputs mi = EvalGraph<RasterFetch>(m, i.uv0, i.uv1, rf);\n";
            ss << "    return PbrShadeSurface(mi, m, i, frontFacing, fragCoord);\n";
            ss << "}\n";
            return ss.str();
        }

        bool WriteShaderFile(const fs::path& path, const std::string& content)
        {
            std::ofstream f(path, std::ios::binary | std::ios::trunc);
            if (!f) return false;
            f << content;
            return static_cast<bool>(f);
        }
    }

    UUID MaterialGraphCodegen::GenerateAndCompile(Material& material)
    {
        if (!material.HasGraph()) return UUID::Invalid();
        if (!FileSystem::HasProject())
        {
            LH_CORE_WARN("MaterialGraphCodegen: no project loaded — cannot emit a graph shader");
            return UUID::Invalid();
        }

        const std::string safe     = SanitizeIdent(material.Handle.ToString());
        const std::string modBase  = "mat_graph_" + safe;
        const std::string consBase = "pbr_graph_" + safe;

        fs::path genDir = FileSystem::ProjectPath("Library/Generated/shaders");
        std::error_code ec; fs::create_directories(genDir, ec);

        const fs::path modPath  = genDir / (modBase  + ".slang");
        const fs::path consPath = genDir / (consBase + ".slang");

        if (!WriteShaderFile(modPath, Lowerer(material.GetGraph()).Run())
         || !WriteShaderFile(consPath, EmitConsumer(modBase)))
        {
            LH_CORE_ERROR("MaterialGraphCodegen: failed writing generated shaders to '{}'", genDir.string());
            return UUID::Invalid();
        }

        SlangCompiler::CompileOutput out = SlangCompiler::CompileReflectStage(consPath);
        if (out.spirv.empty())
        {
            LH_CORE_ERROR("MaterialGraphCodegen: '{}' failed to compile", consBase);
            return UUID::Invalid();
        }

        std::shared_ptr<Shader> shader = Shader::Create(out.stage, out.spirv, consPath);
        if (!shader) return UUID::Invalid();

        ShaderLibrary::Register(consBase, shader);     // ResolveFragSpv matches on the Handle below
        material.SetGraphShaderUUID(shader->Handle);
        LH_CORE_INFO("MaterialGraphCodegen: '{}' compiled ({} SPIR-V words)", consBase, out.spirv.size());
        return shader->Handle;
    }
}
