#include "luthpch.h"
#include "luth/renderer/material/MaterialGraphCodegen.h"
#include "luth/renderer/material/Material.h"
#include "luth/renderer/shader/SlangCompiler.h"
#include "luth/renderer/shader/Shader.h"
#include "luth/renderer/shader/ShaderLibrary.h"
#include "luth/resources/FileSystem.h"
#include "luth/core/diagnostics/Log.h"
#include "luth/jobs/MainThreadPump.h"

#include <fstream>
#include <sstream>
#include <iomanip>
#include <cmath>
#include <functional>
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

        bool IsValueNode(MatNodeType t)
        {
            return t == MatNodeType::ConstFloat || t == MatNodeType::ConstColor || t == MatNodeType::Remap;
        }

        u8 InputCount(MatNodeType t)
        {
            if (t == MatNodeType::Lerp) return 3;
            if (t == MatNodeType::Multiply || t == MatNodeType::Add) return 2;
            return 1;   // Remap / Split (Const / TextureSample have none — slot 0 simply stays unlinked)
        }

        const MatLink* Incoming(const MaterialGraph& g, u32 nodeId, u8 slot)
        {
            for (const auto& l : g.links)
                if (l.toNode == nodeId && l.toSlot == slot) return &l;
            return nullptr;
        }

        // DFS post-order from Output's connected inputs; unreachable nodes dead-strip. invariant: the order
        // depends only on structure (slots + topology), not node IDs — canonicalizes SSA names + param slots.
        std::vector<u32> CanonicalOrder(const MaterialGraph& g)
        {
            std::unordered_map<u32, const MatNode*> byId;
            for (const auto& n : g.nodes) byId[n.id] = &n;
            const MatNode* out = nullptr;
            for (const auto& n : g.nodes) if (n.type == MatNodeType::Output) { out = &n; break; }

            std::vector<u32> order;
            std::unordered_set<u32> done, active;
            std::function<void(u32)> visit = [&](u32 id)
            {
                if (done.count(id) || active.count(id)) return;          // re-emit + cycle guard (hand-edited .mat)
                auto it = byId.find(id);
                if (it == byId.end() || it->second->type == MatNodeType::Output) return;
                active.insert(id);
                const u8 inCount = InputCount(it->second->type);
                for (u8 s = 0; s < inCount; ++s)
                    if (const MatLink* l = Incoming(g, id, s)) visit(l->fromNode);
                active.erase(id);
                done.insert(id);
                order.push_back(id);
            };
            if (out)
                for (u8 s = 0; s < 6; ++s)
                    if (const MatLink* l = Incoming(g, out->id, s)) visit(l->fromNode);
            return order;
        }

        // Graph constants in canonical order — the float4 sequence the generated EvalGraph reads via fetch.Param(k).
        // invariant: same CanonicalOrder + IsValueNode walk as the Lowerer's paramSlot, else the reads desync.
        std::vector<Vec4> BuildParams(const MaterialGraph& g)
        {
            std::unordered_map<u32, const MatNode*> byId;
            for (const auto& n : g.nodes) byId[n.id] = &n;

            std::vector<Vec4> params;
            for (u32 id : CanonicalOrder(g))
            {
                const MatNode* n = byId.at(id);
                if (!IsValueNode(n->type)) continue;
                params.push_back(n->type == MatNodeType::ConstFloat ? Vec4(n->value.x) : n->value);
            }
            return params;
        }

        // Lowers a MaterialGraph to the EvalGraph<F> module body: one float4 SSA local per node (named by canonical
        // visit order, not node ID), value constants read from per-material data (fetch.Param), not baked literals.
        struct Lowerer
        {
            const MaterialGraph& g;
            std::unordered_map<u32, const MatNode*> byId;
            std::vector<u32> order;                  // canonical post-order (reachable, non-Output)
            std::unordered_map<u32, u32> seq;        // node ID -> SSA index (canonical name, no ID leak)
            std::unordered_map<u32, u32> paramSlot;  // value-node ID -> param slot k
            u32 paramCount = 0;

            explicit Lowerer(const MaterialGraph& graph) : g(graph)
            {
                for (const auto& n : g.nodes) byId[n.id] = &n;
                order = CanonicalOrder(g);
                for (u32 i = 0; i < order.size(); ++i)
                {
                    seq[order[i]] = i;
                    if (IsValueNode(byId.at(order[i])->type)) paramSlot[order[i]] = paramCount++;
                }
            }

            std::string Name(u32 nodeId) const { return "n" + std::to_string(seq.at(nodeId)); }

            // float4 expression reading a producer node's output slot (Split exposes channels, else its SSA).
            std::string SourceExpr(const MatNode& prod, u8 fromSlot) const
            {
                if (prod.type == MatNodeType::Split)
                {
                    const char sw[4] = { 'x', 'y', 'z', 'w' };
                    return "float4(" + Name(prod.id) + "." + sw[fromSlot & 3] + ")";
                }
                return Name(prod.id);
            }

            // float4 feeding a consumer node's input slot — linked source, else an identity-aware default.
            std::string Input(const MatNode& n, u8 slot) const
            {
                if (const MatLink* l = Incoming(g, n.id, slot))
                    if (seq.count(l->fromNode))
                        return SourceExpr(*byId.at(l->fromNode), l->fromSlot);

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
                    // Const values are per-material data — ConstFloat broadcasts (BuildParams stores float4(x)).
                    case MatNodeType::ConstFloat:
                    case MatNodeType::ConstColor:    return "fetch.Param(" + std::to_string(paramSlot.at(n.id)) + ")";
                    case MatNodeType::TextureSample: return TexSampleExpr(n.tex);
                    case MatNodeType::Multiply:      return "(" + Input(n, 0) + " * " + Input(n, 1) + ")";
                    case MatNodeType::Add:           return "(" + Input(n, 0) + " + " + Input(n, 1) + ")";
                    case MatNodeType::Lerp:          return "lerp(" + Input(n, 0) + ", " + Input(n, 1) + ", " + Input(n, 2) + ")";
                    // Remap's (inMin,inMax,outMin,outMax) is data; the affine runs in-shader (RemapApply).
                    case MatNodeType::Remap:         return "RemapApply(" + Input(n, 0) + ", fetch.Param(" + std::to_string(paramSlot.at(n.id)) + "))";
                    case MatNodeType::Split:         return Input(n, 0);   // pass-through; channels read via SourceExpr
                    default:                         return "float4(0.0)";
                }
            }

            // Source expr for an Output channel, or empty if unconnected / dangling / dead-stripped.
            std::string OutSrc(const MatNode& out, u8 slot) const
            {
                const MatLink* l = Incoming(g, out.id, slot);
                if (!l || !seq.count(l->fromNode)) return {};
                return SourceExpr(*byId.at(l->fromNode), l->fromSlot);
            }

            std::string Run(const std::string& fnName)
            {
                const MatNode* out = nullptr;
                for (const auto& n : g.nodes) if (n.type == MatNodeType::Output) { out = &n; break; }

                std::ostringstream ss;
                ss << "import material;\n\n";
                ss << "public MaterialInputs " << fnName << "<F : ITexFetch>(GPUMaterialData m, float2 uv0, float2 uv1, F fetch)\n{\n";
                ss << "    MaterialInputs mi = EvalMaterialChannels<F>(m, uv0, uv1, fetch);\n";

                for (u32 id : order)
                    ss << "    float4 " << Name(id) << " = " << NodeRhs(*byId.at(id)) << ";\n";

                if (out)
                {
                    std::string s;
                    if (s = OutSrc(*out, 0); !s.empty()) ss << "    mi.baseColor = " << s << ";\n";
                    if (s = OutSrc(*out, 1); !s.empty()) ss << "    mi.metallic  = " << s << ".x;\n";
                    if (s = OutSrc(*out, 2); !s.empty()) ss << "    mi.roughness = clamp(" << s << ".x, 0.04, 1.0);\n";
                    if (s = OutSrc(*out, 3); !s.empty()) ss << "    mi.normal    = " << s << ".xyz;\n";
                    if (s = OutSrc(*out, 4); !s.empty()) ss << "    mi.occlusion = " << s << ".x;\n";
                    if (s = OutSrc(*out, 5); !s.empty()) ss << "    mi.emissive  = " << s << ".rgb;\n";
                }
                ss << "    return mi;\n}\n";
                return ss.str();
            }
        };

        // The per-material consumer: pbr.slang with the decode swapped to the graph's EvalGraph_<safe>.
        std::string EmitConsumer(const std::string& moduleName, const std::string& fnName)
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
            ss << "    rf.paramBase = i.materialIndex * MAT_GRAPH_STRIDE;\n";
            ss << "    MaterialInputs mi = " << fnName << "<RasterFetch>(m, i.uv0, i.uv1, rf);\n";
            ss << "    return PbrShadeSurface(mi, m, i, frontFacing, fragCoord);\n";
            ss << "}\n";
            return ss.str();
        }

        // The project variant registry — a switch over every graph material's EvalGraph_<safe>, shadowing
        // the engine-default mat_graph_registry on the Slang search path. material_bindings_rt imports it so
        // the shared RT megakernel can decode any graph material. entries are {variantIndex, sanitized-uuid}.
        std::string EmitAggregator(const std::vector<std::pair<u32, std::string>>& entries)
        {
            std::ostringstream ss;
            ss << "import material;\n";
            for (const auto& [v, safe] : entries) ss << "import mat_graph_" << safe << ";\n";
            ss << "\npublic MaterialInputs EvalGraphVariant<F : ITexFetch>(uint variant, GPUMaterialData m, float2 uv0, float2 uv1, F fetch)\n{\n";
            ss << "    switch (variant)\n    {\n";
            for (const auto& [v, safe] : entries)
                ss << "        case " << v << ": return EvalGraph_" << safe << "<F>(m, uv0, uv1, fetch);\n";
            ss << "        default: return EvalMaterialChannels<F>(m, uv0, uv1, fetch);\n";
            ss << "    }\n}\n";
            return ss.str();
        }

        constexpr u32 kMaxGraphVariants = 16;                  // RT megakernel switch-arm cap (occupancy guard)
        std::unordered_map<UUID, u32, UUIDHash> s_Variants;    // material UUID -> RT eval variant (1-based)

        // Recompile the RT megakernels against the regenerated registry. Must run on the main thread —
        // shader reload fires pipeline rebuilds, which is not fiber-safe. One-time per graph edit.
        void ScheduleRtReload()
        {
            MainThreadPump::Post([]() {
                const char* kRtConsumers[] = {
                    "restir_gi_initial.slang", "restir_initial.slang", "rt_reflections.slang",
                    "rt_sun_shadows.slang", "path_trace.slang", "volumetric_inject_scatter.slang"
                };
                for (const char* n : kRtConsumers) ShaderLibrary::Reload(n);
            });
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
        const std::string fnName   = "EvalGraph_" + safe;

        fs::path genDir = FileSystem::ProjectPath("Library/Generated/shaders");
        std::error_code ec; fs::create_directories(genDir, ec);

        const fs::path modPath  = genDir / (modBase  + ".slang");
        const fs::path consPath = genDir / (consBase + ".slang");

        // Value-node count is hard-bounded by the per-material gMatParams stride — beyond it, fetch.Param(k)
        // would read into the next material's region. Fail loud (renders stock) rather than corrupt.
        Lowerer low(material.GetGraph());
        if (low.paramCount > MAT_GRAPH_STRIDE)
        {
            LH_CORE_ERROR("MaterialGraphCodegen: '{}' has {} graph constants (> MAT_GRAPH_STRIDE {}) — renders stock",
                          consBase, low.paramCount, MAT_GRAPH_STRIDE);
            return UUID::Invalid();
        }
        material.SetGraphParams(BuildParams(material.GetGraph()));

        if (!WriteShaderFile(modPath, low.Run(fnName))
         || !WriteShaderFile(consPath, EmitConsumer(modBase, fnName)))
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

        // Assign an RT eval variant (capped). Beyond the cap the material renders correctly in raster but
        // stays stock in RT. Then regenerate the project registry + recompile the RT megakernels so the
        // shared megakernel decodes this graph too.
        u32 variant = 0;
        if (auto it = s_Variants.find(material.Handle); it != s_Variants.end())
            variant = it->second;
        else if (s_Variants.size() < kMaxGraphVariants)
            variant = s_Variants[material.Handle] = static_cast<u32>(s_Variants.size()) + 1;
        else
            LH_CORE_WARN("MaterialGraphCodegen: RT variant cap ({}) reached — '{}' renders stock in RT", kMaxGraphVariants, consBase);

        material.SetGraphVariant(variant);
        material.UpdateGPUData();   // pack the variant into flags 8-15 for the per-frame material SSBO upload

        if (variant != 0)
        {
            std::vector<std::pair<u32, std::string>> entries;
            for (const auto& [uuid, v] : s_Variants) entries.emplace_back(v, SanitizeIdent(uuid.ToString()));
            WriteShaderFile(genDir / "mat_graph_registry.slang", EmitAggregator(entries));
            ScheduleRtReload();
        }

        LH_CORE_INFO("MaterialGraphCodegen: '{}' compiled (variant {}, {} SPIR-V words)", consBase, variant, out.spirv.size());
        return shader->Handle;
    }

    // Value-edit fast path: re-extract the cached constants from node values with no compile or variant change.
    // The generated shader is unchanged (structure is identical), so the edit lands purely as gMatParams data.
    void MaterialGraphCodegen::RefreshParams(Material& material)
    {
        material.SetGraphParams(material.HasGraph() ? BuildParams(material.GetGraph()) : std::vector<Vec4>{});
    }
}
