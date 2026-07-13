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
        // fetch.Sample(...) for a TextureSample node's map slot. Maps carrying a per-map UV-index bit follow
        // it; the rest sample UV0; mirrors EvalMaterialChannels in material.slang.
        std::string TexSampleExpr(u32 tex)
        {
            switch (static_cast<MapType>(tex))
            {
                case MapType::Diffuse:   return "fetch.Sample(m.diffuseIndex, SelectUV(m.flags, UV_SHIFT_DIFFUSE, uv0, uv1))";
                // Decode to a signed tangent normal (Z reconstructed, so BC5 RG normals work) so routing it
                // to Output.normal honors the mi.normal convention.
                case MapType::Normal:    return "float4(DecodeTangentNormal(fetch.Sample(m.normalIndex, SelectUV(m.flags, UV_SHIFT_NORMAL, uv0, uv1)).rgb), 0.0)";
                case MapType::Metalness:
                case MapType::Roughness: return "fetch.Sample(m.metalRoughIndex, SelectUV(m.flags, UV_SHIFT_METALROUGH, uv0, uv1))";
                case MapType::Occlusion: return "fetch.Sample(m.occlusionIndex, SelectUV(m.flags, UV_SHIFT_OCCLUSION, uv0, uv1))";
                case MapType::Emissive:  return "fetch.Sample(m.emissiveIndex, uv0)";
                case MapType::Height:    return "fetch.Sample(m.heightIndex, uv0)";
                case MapType::Thickness: return "fetch.Sample(m.thicknessIndex, uv0)";
                case MapType::Subsurface: return "fetch.Sample(m.subsurfaceIndex, uv0)";
                // MapType::Alpha lost its GPU slot (repurposed for subsurface); legacy nodes sample black.
                default:                 return "float4(0.0)";
            }
        }

        // Same map->index routing, sampling at a graph-provided UV expression (a linked TextureSample UV pin).
        std::string TexSampleExprAt(u32 tex, const std::string& uvExpr)
        {
            const char* idx = nullptr;
            switch (static_cast<MapType>(tex))
            {
                case MapType::Diffuse:   idx = "m.diffuseIndex";    break;
                case MapType::Normal:    return "float4(DecodeTangentNormal(fetch.Sample(m.normalIndex, (" + uvExpr + ").xy).rgb), 0.0)";
                case MapType::Metalness:
                case MapType::Roughness: idx = "m.metalRoughIndex"; break;
                case MapType::Occlusion: idx = "m.occlusionIndex";  break;
                case MapType::Emissive:  idx = "m.emissiveIndex";   break;
                case MapType::Height:    idx = "m.heightIndex";     break;
                case MapType::Thickness: idx = "m.thicknessIndex";  break;
                case MapType::Subsurface: idx = "m.subsurfaceIndex"; break;
                default:                 return "float4(0.0)";
            }
            return "fetch.Sample(" + std::string(idx) + ", (" + uvExpr + ").xy)";
        }

        // Raw bindless-index token for a map slot: Triplanar samples one map three times itself, so it needs
        // the index expression, not a ready-made sample. (Normal maps get no tangent handling here.)
        const char* TexSampleIndexToken(u32 tex)
        {
            switch (static_cast<MapType>(tex))
            {
                case MapType::Diffuse:   return "m.diffuseIndex";
                case MapType::Normal:    return "m.normalIndex";
                case MapType::Metalness:
                case MapType::Roughness: return "m.metalRoughIndex";
                case MapType::Occlusion: return "m.occlusionIndex";
                case MapType::Emissive:  return "m.emissiveIndex";
                case MapType::Height:    return "m.heightIndex";
                case MapType::Thickness: return "m.thicknessIndex";
                case MapType::Subsurface: return "m.subsurfaceIndex";
                default:                 return "m.diffuseIndex";
            }
        }

        bool IsValueNode(MatNodeType t)
        {
            return t == MatNodeType::ConstFloat || t == MatNodeType::ConstColor || t == MatNodeType::Remap
                || t == MatNodeType::Noise      || t == MatNodeType::Fresnel
                || t == MatNodeType::Triplanar  || t == MatNodeType::DetailNormal
                || t == MatNodeType::Parallax   || t == MatNodeType::Decal
                || t == MatNodeType::PropertyRef;
        }

        u8 InputCount(MatNodeType t)
        {
            switch (t)
            {
                case MatNodeType::MakeLayer:
                    return 20;   // 6 base + 4 coat/aniso (6-9) + 5 transmission (10-14) + 2 sheen (15-16) + 3 subsurface (17-19)
                case MatNodeType::Custom:
                    return 4;
                case MatNodeType::Lerp: case MatNodeType::LayerBlend:
                    return 3;
                case MatNodeType::Multiply: case MatNodeType::Add: case MatNodeType::StaticSwitch:
                case MatNodeType::Subtract: case MatNodeType::Divide: case MatNodeType::Power:
                case MatNodeType::Min: case MatNodeType::Max: case MatNodeType::Dot:
                case MatNodeType::DetailNormal: case MatNodeType::Decal:
                    return 2;
                default:
                    return 1;   // Remap / Split / Noise / TextureSample UV pin (value + context nodes leave slot 0 unlinked)
            }
        }

        const MatLink* Incoming(const MaterialGraph& g, u32 nodeId, u8 slot)
        {
            for (const auto& l : g.links)
                if (l.toNode == nodeId && l.toSlot == slot) return &l;
            return nullptr;
        }

        // DFS post-order from Output's connected inputs; unreachable nodes dead-strip. invariant: the order
        // depends only on structure (slots + topology), not node IDs; canonicalizes SSA names + param slots.
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
                const MatNode* n = it->second;
                if (n->type == MatNodeType::StaticSwitch)
                {
                    // Compile-time select: only the chosen branch is reachable, so the other side
                    // dead-strips from BOTH the emitted source and BuildParams (they share this walk).
                    // Flipping the state changes the reachable set -> new canonical source -> new structure.
                    if (const MatLink* l = Incoming(g, id, n->value.x != 0.0f ? 1 : 0)) visit(l->fromNode);
                }
                else
                {
                    const u8 inCount = InputCount(n->type);
                    for (u8 s = 0; s < inCount; ++s)
                        if (const MatLink* l = Incoming(g, id, s)) visit(l->fromNode);
                }
                active.erase(id);
                done.insert(id);
                order.push_back(id);
            };
            if (out)
            {
                for (u8 s = 0; s < 6; ++s)
                    if (const MatLink* l = Incoming(g, out->id, s)) visit(l->fromNode);
                // Surface (bundle) slot 6: appended after the channel slots so pre-slot-6 graphs stay order-stable.
                if (const MatLink* l = Incoming(g, out->id, 6)) visit(l->fromNode);
                // Ext channels (coat/aniso 7-10 + transmission 11-15 + sheen 16-17 + subsurface 18-20): appended
                // last, so a pre-ext graph (nothing on those slots) keeps its visit order + structure hash unchanged.
                for (u8 s = 7; s < 21; ++s)
                    if (const MatLink* l = Incoming(g, out->id, s)) visit(l->fromNode);
            }
            return order;
        }

        // Graph constants in canonical order: the float4 sequence the generated EvalGraph reads via fetch.Param(k).
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
                if (n->type == MatNodeType::PropertyRef)
                {
                    // Value flows from the referenced Blackboard property (Float broadcasts; Color = rgba).
                    const MaterialProperty* p = FindProperty(g, n->tex);
                    params.push_back(p && p->type == MatPropType::Color ? p->value : Vec4(p ? p->value.x : 0.0f));
                    continue;
                }
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

            // float4 feeding a consumer node's input slot: linked source, else an identity-aware default.
            std::string Input(const MatNode& n, u8 slot) const
            {
                if (const MatLink* l = Incoming(g, n.id, slot))
                    if (seq.count(l->fromNode))
                        return SourceExpr(*byId.at(l->fromNode), l->fromSlot);

                switch (n.type)
                {
                    case MatNodeType::Multiply: return "float4(1.0)";
                    case MatNodeType::Divide:
                    case MatNodeType::Power:    return slot == 1 ? "float4(1.0)" : "float4(0.0)";   // identity operands
                    case MatNodeType::Lerp:     return slot == 2 ? "float4(0.5)" : "float4(0.0)";
                    case MatNodeType::DetailNormal: return "float4(0.0, 0.0, 1.0, 0.0)";   // identity tangent normal
                    default:                    return "float4(0.0)";
                }
            }

            // MaterialInputs feeding a LayerBlend bundle slot: the linked layer local, else a fresh stock base
            // layer. The editor's AddLink guard keeps float4 producers out of these slots.
            std::string InputLayer(const MatNode& n, u8 slot) const
            {
                if (const MatLink* l = Incoming(g, n.id, slot))
                    if (seq.count(l->fromNode))
                        return SourceExpr(*byId.at(l->fromNode), l->fromSlot);
                return "EvalMaterialChannels<F>(m, uv0, uv1, fetch)";
            }

            std::string NodeRhs(const MatNode& n) const
            {
                switch (n.type)
                {
                    // Const values are per-material data; ConstFloat broadcasts (BuildParams stores float4(x)).
                    case MatNodeType::ConstFloat:
                    case MatNodeType::ConstColor:
                    case MatNodeType::PropertyRef:   return "fetch.Param(" + std::to_string(paramSlot.at(n.id)) + ")";
                    case MatNodeType::TextureSample:
                    {
                        // A linked UV pin overrides the material's per-map UV-set selection.
                        if (const MatLink* l = Incoming(g, n.id, 0))
                            if (seq.count(l->fromNode))
                                return TexSampleExprAt(n.tex, SourceExpr(*byId.at(l->fromNode), l->fromSlot));
                        return TexSampleExpr(n.tex);
                    }
                    case MatNodeType::Multiply:      return "(" + Input(n, 0) + " * " + Input(n, 1) + ")";
                    case MatNodeType::Add:           return "(" + Input(n, 0) + " + " + Input(n, 1) + ")";
                    case MatNodeType::Subtract:      return "(" + Input(n, 0) + " - " + Input(n, 1) + ")";
                    case MatNodeType::Divide:        return "(" + Input(n, 0) + " / " + Input(n, 1) + ")";
                    case MatNodeType::Power:         return "pow(max(" + Input(n, 0) + ", float4(0.0)), " + Input(n, 1) + ")";
                    case MatNodeType::Min:           return "min(" + Input(n, 0) + ", " + Input(n, 1) + ")";
                    case MatNodeType::Max:           return "max(" + Input(n, 0) + ", " + Input(n, 1) + ")";
                    case MatNodeType::Dot:           return "float4(dot(" + Input(n, 0) + ".xyz, " + Input(n, 1) + ".xyz))";
                    case MatNodeType::Abs:           return "abs(" + Input(n, 0) + ")";
                    case MatNodeType::Saturate:      return "saturate(" + Input(n, 0) + ")";
                    case MatNodeType::OneMinus:      return "(float4(1.0) - " + Input(n, 0) + ")";
                    case MatNodeType::UV:            return n.tex != 0u ? "float4(uv1, 0.0, 0.0)" : "float4(uv0, 0.0, 0.0)";
                    case MatNodeType::Noise:
                    {
                        // Unlinked coord defaults to uv0 (the float4(0) default would sample a constant).
                        std::string uv = "float4(uv0, 0.0, 0.0)";
                        if (const MatLink* l = Incoming(g, n.id, 0))
                            if (seq.count(l->fromNode)) uv = SourceExpr(*byId.at(l->fromNode), l->fromSlot);
                        return "GraphNoise((" + uv + ").xy, fetch.Param(" + std::to_string(paramSlot.at(n.id)) + "))";
                    }
                    case MatNodeType::WorldPos:      return "float4(fetch.WorldPos(), 0.0)";
                    case MatNodeType::ViewDir:       return "float4(fetch.ViewDir(), 0.0)";
                    case MatNodeType::Time:          return "float4(fetch.Time())";
                    case MatNodeType::Fresnel:       return "float4(pow(1.0 - fetch.NdotV(), max(fetch.Param(" + std::to_string(paramSlot.at(n.id)) + ").x, 1e-3)))";
                    case MatNodeType::Lerp:          return "lerp(" + Input(n, 0) + ", " + Input(n, 1) + ", " + Input(n, 2) + ")";
                    // Remap's (inMin,inMax,outMin,outMax) is data; the affine runs in-shader (RemapApply).
                    case MatNodeType::Remap:         return "RemapApply(" + Input(n, 0) + ", fetch.Param(" + std::to_string(paramSlot.at(n.id)) + "))";
                    case MatNodeType::Split:         return Input(n, 0);   // pass-through; channels read via SourceExpr
                    case MatNodeType::StaticSwitch:  return Input(n, n.value.x != 0.0f ? 1 : 0);   // compile-time select
                    case MatNodeType::Triplanar:     return "GraphTriplanar<F>(fetch, " + std::string(TexSampleIndexToken(n.tex)) + ", fetch.WorldPos(), fetch.WorldNormal(), fetch.Param(" + std::to_string(paramSlot.at(n.id)) + ").x)";
                    case MatNodeType::DetailNormal:  return "GraphDetailNormal(" + Input(n, 0) + ", " + Input(n, 1) + ", fetch.Param(" + std::to_string(paramSlot.at(n.id)) + ").x)";
                    case MatNodeType::LayerBlend:    return "GraphLayerBlend(" + InputLayer(n, 0) + ", " + InputLayer(n, 1) + ", (" + Input(n, 2) + ").x)";
                    case MatNodeType::Parallax:
                    {
                        // Optional UV input (slot 0) defaults to uv0; always marches the material's height map.
                        std::string uv = "float4(uv0, 0.0, 0.0)";
                        if (const MatLink* l = Incoming(g, n.id, 0))
                            if (seq.count(l->fromNode)) uv = SourceExpr(*byId.at(l->fromNode), l->fromSlot);
                        return "GraphParallax<F>(fetch, m.heightIndex, (" + uv
                             + ").xy, fetch.TangentViewDir(), fetch.Param(" + std::to_string(paramSlot.at(n.id)) + "))";
                    }
                    case MatNodeType::Decal:
                    {
                        // Base layer (slot 0, stock fallback via InputLayer); optional UV (slot 1) defaults uv0.
                        std::string uv = "float4(uv0, 0.0, 0.0)";
                        if (const MatLink* l = Incoming(g, n.id, 1))
                            if (seq.count(l->fromNode)) uv = SourceExpr(*byId.at(l->fromNode), l->fromSlot);
                        return "GraphDecal<F>(fetch, m.decalIndex, " + InputLayer(n, 0) + ", (" + uv
                             + ").xy, fetch.Param(" + std::to_string(paramSlot.at(n.id)) + "))";
                    }
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

            // Per-channel overrides into a MaterialInputs target from a node's channel input slots. Shared by
            // Output's terminal mi and MakeLayer's local. extBase = the slot where the clear-coat/aniso channels
            // begin (Output = 7, after the Surface slot; MakeLayer = 6). Appended after the six base channels so
            // a pre-ext graph (nothing on those slots) emits byte-identically -> its structure hash is unchanged.
            void EmitChannelOverrides(std::ostringstream& ss, const std::string& tgt, const MatNode& n, u8 extBase) const
            {
                std::string s;
                if (s = OutSrc(n, 0); !s.empty()) ss << "    " << tgt << ".baseColor = " << s << ";\n";
                if (s = OutSrc(n, 1); !s.empty()) ss << "    " << tgt << ".metallic  = " << s << ".x;\n";
                if (s = OutSrc(n, 2); !s.empty()) ss << "    " << tgt << ".roughness = clamp(" << s << ".x, 0.04, 1.0);\n";
                if (s = OutSrc(n, 3); !s.empty()) ss << "    " << tgt << ".normal    = " << s << ".xyz;\n";
                if (s = OutSrc(n, 4); !s.empty()) ss << "    " << tgt << ".occlusion = " << s << ".x;\n";
                if (s = OutSrc(n, 5); !s.empty()) ss << "    " << tgt << ".emissive  = " << s << ".rgb;\n";
                if (s = OutSrc(n, extBase + 0); !s.empty()) ss << "    " << tgt << ".clearcoat          = " << s << ".x;\n";
                if (s = OutSrc(n, extBase + 1); !s.empty()) ss << "    " << tgt << ".clearcoatRoughness = clamp(" << s << ".x, 0.04, 1.0);\n";
                if (s = OutSrc(n, extBase + 2); !s.empty()) ss << "    " << tgt << ".anisotropy         = clamp(" << s << ".x, -1.0, 1.0);\n";
                if (s = OutSrc(n, extBase + 3); !s.empty()) ss << "    " << tgt << ".anisotropyRotation = " << s << ".x;\n";
                if (s = OutSrc(n, extBase + 4); !s.empty()) ss << "    " << tgt << ".transmission        = clamp(" << s << ".x, 0.0, 1.0);\n";
                if (s = OutSrc(n, extBase + 5); !s.empty()) ss << "    " << tgt << ".ior                 = clamp(" << s << ".x, 1.0, 4.0);\n";
                if (s = OutSrc(n, extBase + 6); !s.empty()) ss << "    " << tgt << ".thickness           = max(" << s << ".x, 0.0);\n";
                if (s = OutSrc(n, extBase + 7); !s.empty()) ss << "    " << tgt << ".attenuationColor    = " << s << ".rgb;\n";
                if (s = OutSrc(n, extBase + 8); !s.empty()) ss << "    " << tgt << ".attenuationDistance = " << s << ".x;\n";
                if (s = OutSrc(n, extBase + 9); !s.empty()) ss << "    " << tgt << ".sheenColor          = " << s << ".rgb;\n";
                if (s = OutSrc(n, extBase + 10); !s.empty()) ss << "    " << tgt << ".sheenRoughness      = clamp(" << s << ".x, 0.04, 1.0);\n";
                if (s = OutSrc(n, extBase + 11); !s.empty()) ss << "    " << tgt << ".subsurfaceColor     = " << s << ".rgb;\n";
                if (s = OutSrc(n, extBase + 12); !s.empty()) ss << "    " << tgt << ".subsurfaceRadius    = max(" << s << ".x, 0.0);\n";
                if (s = OutSrc(n, extBase + 13); !s.empty()) ss << "    " << tgt << ".subsurfaceThickness = max(" << s << ".x, 0.0);\n";
            }

            std::string Run(const std::string& fnName)
            {
                const MatNode* out = nullptr;
                for (const auto& n : g.nodes) if (n.type == MatNodeType::Output) { out = &n; break; }

                std::ostringstream ss;
                ss << "import material;\n";
                // graph_lib / effects imported only when used, so pre-existing structures' hash stays stable.
                for (u32 id : order)
                    if (byId.at(id)->type == MatNodeType::Noise) { ss << "import graph_lib;\n"; break; }
                for (u32 id : order)
                {
                    const MatNodeType t = byId.at(id)->type;
                    if (t == MatNodeType::Triplanar || t == MatNodeType::DetailNormal || IsLayerNode(t)
                        || t == MatNodeType::Parallax)
                    { ss << "import effects;\n"; break; }
                }
                ss << "\n";
                ss << "public MaterialInputs " << fnName << "<F : ITexFetch>(GPUMaterialData m, float2 uv0, float2 uv1, F fetch)\n{\n";
                ss << "    MaterialInputs mi = EvalMaterialChannels<F>(m, uv0, uv1, fetch);\n";

                for (u32 id : order)
                {
                    const MatNode& n = *byId.at(id);
                    if (n.type == MatNodeType::Custom)
                    {
                        // Block-scoped so the user code is a plain float4 expression over locals a..d.
                        ss << "    float4 " << Name(id) << ";\n";
                        ss << "    { float4 a = " << Input(n, 0) << "; float4 b = " << Input(n, 1)
                           << "; float4 c = " << Input(n, 2) << "; float4 d = " << Input(n, 3) << "; "
                           << Name(id) << " = (" << (n.code.empty() ? "float4(0.0)" : n.code) << "); }\n";
                    }
                    else if (n.type == MatNodeType::MakeLayer)
                    {
                        // A "layer": stock base + only the connected channel overrides (Make Material Attributes).
                        ss << "    MaterialInputs " << Name(id) << " = EvalMaterialChannels<F>(m, uv0, uv1, fetch);\n";
                        EmitChannelOverrides(ss, Name(id), n, 6);   // MakeLayer has no Surface slot -> ext at 6
                    }
                    else if (IsLayerNode(n.type))
                        ss << "    MaterialInputs " << Name(id) << " = " << NodeRhs(n) << ";\n";
                    else
                        ss << "    float4 " << Name(id) << " = " << NodeRhs(n) << ";\n";
                }

                if (out)
                {
                    // Surface (bundle) slot 6: a connected layer replaces the stock base before channel overrides.
                    if (const MatLink* l = Incoming(g, out->id, 6); l && seq.count(l->fromNode))
                        ss << "    mi = " << SourceExpr(*byId.at(l->fromNode), l->fromSlot) << ";\n";
                    EmitChannelOverrides(ss, "mi", *out, 7);   // Output ext channels sit after Surface (slot 6)
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
            ss << "    rf.worldPos  = i.worldPos;\n";
            ss << "    rf.viewDir   = normalize(ubo.cameraPos - i.worldPos);\n";
            ss << "    rf.time      = ubo.time;\n";
            ss << "    rf.ndotv     = saturate(dot(normalize(i.normal), rf.viewDir));\n";
            ss << "    rf.worldNormal = normalize(i.normal);\n";
            ss << "    rf.tangentViewDir = float3(dot(rf.viewDir, i.T), dot(rf.viewDir, i.B), dot(rf.viewDir, i.N));\n";
            ss << "    MaterialInputs mi = " << fnName << "<RasterFetch>(m, i.uv0, i.uv1, rf);\n";
            ss << "    return PbrShadeSurface(mi, m, i, frontFacing, fragCoord);\n";
            ss << "}\n";
            return ss.str();
        }

        // The per-material preview consumer: graph decode + a Lambert+ambient mini-shade for the editor
        // thumbnail/inspector. Self-contained (PreviewFetch + per-call UBO + bindless); no MaterialSystem set.
        std::string EmitPreviewConsumer(const std::string& moduleName, const std::string& fnName)
        {
            std::ostringstream ss;
            ss << "import material;\n";
            ss << "import material_bindings_preview;\n";
            ss << "import " << moduleName << ";\n\n";
            ss << "struct FOut { float4 color : SV_Target0; }\n\n";
            ss << "[shader(\"fragment\")]\n";
            ss << "FOut main(PreviewVaryings v)\n{\n";
            ss << "    GPUMaterialData m = gPreview.m;\n";
            ss << "    PreviewFetch pf;\n";
            ss << "    pf.ndotv = saturate(dot(normalize(v.normal), pf.viewDir));\n";   // canned front view; time stays 0
            ss << "    MaterialInputs mi = " << fnName << "<PreviewFetch>(m, v.uv0, v.uv1, pf);\n";
            ss << "    float3 N = normalize(v.normal);\n";
            ss << "    if (any(mi.normal != float3(0.0, 0.0, 1.0)) && length(v.tangent) > 1e-6)\n";
            ss << "    {\n";
            ss << "        float3 T = normalize(v.tangent - dot(v.tangent, N) * N);\n";
            ss << "        N = ApplyTangentNormal(mi.normal, T, cross(N, T), N);\n";
            ss << "    }\n";
            ss << "    const float3 L = normalize(float3(0.5, 1.0, 0.7));\n";
            ss << "    float3 base = mi.baseColor.rgb;\n";
            ss << "    float diffuse = max(dot(N, L), 0.0);\n";
            ss << "    FOut o;\n";
            ss << "    o.color = float4(base * 0.25 + diffuse * base * 0.75 + mi.emissive, 1.0);\n";
            ss << "    return o;\n";
            ss << "}\n";
            return ss.str();
        }

        // The project variant registry: a switch over every distinct structure's EvalGraph_<hash>, shadowing
        // the engine-default mat_graph_registry on the Slang search path. material_bindings_rt imports it so
        // the shared RT megakernel can decode any graph material. Entries are {variantIndex, structure-hash hex}.
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

        constexpr u32 kMaxGraphVariants = 64;   // RT megakernel switch-arm cap; counts distinct structures (flags 8-15 allow 256)

        struct StructInfo { u32 variant; UUID shaderUUID; UUID previewUUID; std::string canonSrc; };
        std::unordered_map<u64, StructInfo> s_Structures;   // structure hash -> shared variant + compiled shader

        // FNV-1a over the canonical (value-free, ID-free) module source: the structure key. Materials with the
        // same graph shape hash equal and share one shader + variant; their constants differ only in gMatParams.
        u64 Fnv1a(const std::string& s)
        {
            u64 h = 1469598103934665603ull;
            for (unsigned char c : s) { h ^= c; h *= 1099511628211ull; }
            return h;
        }

        std::string HexU64(u64 v)
        {
            std::ostringstream o; o << std::hex << std::setw(16) << std::setfill('0') << v;
            return o.str();
        }

        // Coalesces reloads: N new structures before the next main-thread drain (scene load) trigger ONE
        // 8-shader batch, not N. Safe because the registry file is regenerated before every schedule call,
        // so the drained reload compiles against the superset.
        std::atomic<bool> s_ReloadPending{ false };

        // Reload every shared consumer of the variant registry (RT megakernels + the two raster
        // transparent shaders) against the regenerated version.
        void ScheduleGraphConsumerReload()
        {
            if (s_ReloadPending.exchange(true)) return;
            MainThreadPump::Post([]() {
                s_ReloadPending.store(false);   // clear first: a compile landing mid-reload re-posts a fresh batch
                const char* kGraphConsumers[] = {
                    "restir_gi_initial.slang", "restir_initial.slang", "rt_reflections.slang",
                    "rt_sun_shadows.slang", "path_trace.slang", "volumetric_inject_scatter.slang",
                    "pbr_transparent.slang", "pbr_oit_store.slang"
                };
                for (const char* n : kGraphConsumers) ShaderLibrary::Reload(n);
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
        LH_PROFILE_FUNCTION();
        if (!material.HasGraph()) return UUID::Invalid();
        if (!FileSystem::HasProject())
        {
            LH_LOG(Renderer, warn, "MaterialGraphCodegen: no project loaded - cannot emit a graph shader");
            return UUID::Invalid();
        }

        // Custom-node sandbox: reject stage-divergent constructs before emitting anything, so the material
        // falls to stock in EVERY tier (a fragment-only intrinsic would compile raster but break the RT
        // megakernels -> raster!=RT). invariant: raster == RT even for user code.
        for (const MatNode& n : material.GetGraph().nodes)
            if (n.type == MatNodeType::Custom)
                if (const char* bad = ValidateCustomCode(n.code))
                {
                    LH_LOG(Renderer, error, "MaterialGraphCodegen: '{}' Custom node uses banned construct '{}' - renders stock",
                                  material.Handle.ToString(), bad);
                    return UUID::Invalid();
                }

        // Value-node count is hard-bounded by the per-material gMatParams stride; beyond it, fetch.Param(k)
        // would read into the next material's region. Fail loud (renders stock) rather than corrupt.
        Lowerer low(material.GetGraph());
        if (low.paramCount > MAT_GRAPH_STRIDE)
        {
            LH_LOG(Renderer, error, "MaterialGraphCodegen: '{}' has {} graph constants (> MAT_GRAPH_STRIDE {}) - renders stock",
                          material.Handle.ToString(), low.paramCount, MAT_GRAPH_STRIDE);
            return UUID::Invalid();
        }
        material.SetGraphParams(BuildParams(material.GetGraph()));

        // Structure key: the canonical module source under a fixed fn name (value-free + ID-free). Structurally
        // identical materials hash equal and share the compiled shader + RT variant; their constants live in data.
        const std::string canonSrc   = low.Run("EvalGraph");
        const u64         structHash = Fnv1a(canonSrc);

        // Hash hit: this structure was already compiled; reuse its shader + variant, skip codegen and the RT
        // reload entirely. The source memcmp rules out a (vanishingly rare) hash collision before reusing.
        if (auto it = s_Structures.find(structHash); it != s_Structures.end() && it->second.canonSrc == canonSrc)
        {
            const StructInfo& si = it->second;
            material.SetGraphShaderUUID(si.shaderUUID);
            material.SetGraphPreviewShaderUUID(si.previewUUID);
            material.SetGraphVariant(si.variant);
            material.UpdateGPUData();
            return si.shaderUUID;
        }

        const std::string hashHex  = HexU64(structHash);
        const std::string modBase  = "mat_graph_" + hashHex;
        const std::string consBase = "pbr_graph_" + hashHex;
        const std::string fnName   = "EvalGraph_" + hashHex;

        fs::path genDir = FileSystem::ProjectPath("Library/Generated/shaders");
        std::error_code ec; fs::create_directories(genDir, ec);
        const fs::path modPath  = genDir / (modBase  + ".slang");
        const fs::path consPath = genDir / (consBase + ".slang");

        if (!WriteShaderFile(modPath, low.Run(fnName))
         || !WriteShaderFile(consPath, EmitConsumer(modBase, fnName)))
        {
            LH_LOG(Renderer, error, "MaterialGraphCodegen: failed writing generated shaders to '{}'", genDir.string());
            return UUID::Invalid();
        }

        SlangCompiler::CompileOutput out = SlangCompiler::CompileReflectStage(consPath);
        if (out.spirv.empty())
        {
            LH_LOG(Renderer, error, "MaterialGraphCodegen: '{}' failed to compile", consBase);
            return UUID::Invalid();
        }

        std::shared_ptr<Shader> shader = Shader::Create(out.stage, out.spirv, consPath);
        if (!shader) return UUID::Invalid();
        ShaderLibrary::Register(consBase, shader);     // ResolveFragSpv matches on the Handle below

        // Preview consumer (editor thumbnail/inspector). Non-fatal: on failure the material still renders
        // in the viewport; only the editor preview falls back to the stock Lambert shader.
        const std::string prevBase = "thumbnail_graph_" + hashHex;
        const fs::path    prevPath = genDir / (prevBase + ".slang");
        UUID previewUUID = UUID::Invalid();
        if (WriteShaderFile(prevPath, EmitPreviewConsumer(modBase, fnName)))
        {
            SlangCompiler::CompileOutput pout = SlangCompiler::CompileReflectStage(prevPath);
            if (!pout.spirv.empty())
                if (auto psh = Shader::Create(pout.stage, pout.spirv, prevPath))
                {
                    ShaderLibrary::Register(prevBase, psh);
                    previewUUID = psh->Handle;
                }
        }
        if (!previewUUID.IsValid())
            LH_LOG(Renderer, warn, "MaterialGraphCodegen: '{}' preview consumer failed - editor preview stays stock", prevBase);

        // New structure -> assign an RT eval variant (capped on distinct structures). Beyond the cap the
        // material renders correctly in raster (per-structure shader) but stays stock in RT.
        u32 variant = 0;
        if (s_Structures.size() < kMaxGraphVariants)
            variant = static_cast<u32>(s_Structures.size()) + 1;
        else
            LH_LOG(Renderer, warn, "MaterialGraphCodegen: RT variant cap ({}) reached - '{}' renders stock in RT", kMaxGraphVariants, consBase);

        s_Structures[structHash] = StructInfo{ variant, shader->Handle, previewUUID, canonSrc };
        material.SetGraphShaderUUID(shader->Handle);
        material.SetGraphPreviewShaderUUID(previewUUID);
        material.SetGraphVariant(variant);
        material.UpdateGPUData();   // pack the variant into flags 8-15 for the per-frame material SSBO upload

        // Regenerate the project registry + recompile the RT megakernels so the shared megakernel decodes this
        // new structure. Only fires on a brand-new structure; value edits and clones hash-hit above.
        if (variant != 0)
        {
            std::vector<std::pair<u32, std::string>> entries;
            for (const auto& [hash, si] : s_Structures)
                if (si.variant != 0) entries.emplace_back(si.variant, HexU64(hash));
            WriteShaderFile(genDir / "mat_graph_registry.slang", EmitAggregator(entries));
            ScheduleGraphConsumerReload();
        }

        LH_LOG(Renderer, info, "MaterialGraphCodegen: '{}' compiled (variant {}, {} SPIR-V words)", consBase, variant, out.spirv.size());
        return shader->Handle;
    }

    // Value-edit fast path: re-extract the cached constants from node values with no compile or variant change.
    // The generated shader is unchanged (structure is identical), so the edit lands purely as gMatParams data.
    void MaterialGraphCodegen::RefreshParams(Material& material)
    {
        LH_PROFILE_FUNCTION();
        material.SetGraphParams(material.HasGraph() ? BuildParams(material.GetGraph()) : std::vector<Vec4>{});
    }

    const char* MaterialGraphCodegen::ValidateCustomCode(const std::string& code)
    {
        static const char* kBanned[] = {
            "ddx", "ddy", "fwidth", "discard", "gTextures", "gMaterials", "gMatParams",
            "RWTexture", "import", "[shader"
        };
        for (const char* b : kBanned)
            if (code.find(b) != std::string::npos) return b;
        return nullptr;
    }
}
