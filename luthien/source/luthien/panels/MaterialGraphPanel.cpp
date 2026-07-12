#include "lepch.h"
#include "luthien/panels/MaterialGraphPanel.h"
#include "luthien/EditorSelection.h"
#include "luthien/widgets/Charts.h"
#include "luthien/widgets/Icons.h"
#include "luth/renderer/material/Material.h"
#include "luth/renderer/material/MaterialGraphCodegen.h"
#include "luth/resources/AssetManager.h"
#include "luth/resources/AssetDatabase.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <string>

namespace Luth
{
    namespace
    {
        // Per-node-type pin layout + label. Indexed by MatNodeType. Pin-name arrays are static so the
        // GraphEditor::Template's const char** pointers stay valid for the canvas's lifetime.
        const char* kIn_AB[]    = { "A", "B" };
        const char* kIn_ABT[]   = { "A", "B", "T" };
        const char* kIn_OffOn[] = { "Off", "On" };
        const char* kIn_UV[]    = { "UV" };
        const char* kIn_ABCD[]  = { "A", "B", "C", "D" };
        const char* kIn_In[]    = { "in" };
        const char* kIn_RGBA[]  = { "rgba" };
        const char* kOut_1[]    = { "out" };
        const char* kOut_RGBA[] = { "rgba" };
        const char* kOut_Split[]= { "R", "G", "B", "A" };
        const char* kIn_BaseDet[] = { "Base", "Detail" };
        const char* kIn_BaseUV[]  = { "Base", "UV" };
        const char* kIn_BTM[]     = { "Bottom", "Top", "Mask" };
        const char* kOut_Layer[]  = { "Layer" };
        // Ext channels appended to Output (after Surface, slots 7-15) and MakeLayer (slots 6-14): clear-coat/
        // aniso then dielectric transmission (Transmission/IOR/Thickness/AttenColor/AttenDist).
        const char* kIn_Out16[]   = { "BaseColor", "Metallic", "Roughness", "Normal", "AO", "Emissive",
                                      "Surface", "ClearCoat", "CoatRough", "Anisotropy", "AnisoRot",
                                      "Transmission", "IOR", "Thickness", "AttenColor", "AttenDist" };
        const char* kIn_Layer15[] = { "BaseColor", "Metallic", "Roughness", "Normal", "AO", "Emissive",
                                      "ClearCoat", "CoatRough", "Anisotropy", "AnisoRot",
                                      "Transmission", "IOR", "Thickness", "AttenColor", "AttenDist" };

        struct TypeInfo
        {
            const char*  name;
            ImU32        header;
            const char** inNames;  u8 inCount;
            const char** outNames; u8 outCount;
        };

        const TypeInfo kTypes[] = {
            { "Float",    IM_COL32( 70, 90,120,255), nullptr,  0, kOut_1,     1 },  // ConstFloat
            { "Color",    IM_COL32(120, 90, 70,255), nullptr,  0, kOut_RGBA,  1 },  // ConstColor
            { "Texture",  IM_COL32( 70,120, 90,255), kIn_UV,   1, kOut_RGBA,  1 },  // TextureSample (UV pin optional)
            { "Multiply", IM_COL32( 90, 90, 90,255), kIn_AB,   2, kOut_1,     1 },  // Multiply
            { "Add",      IM_COL32( 90, 90, 90,255), kIn_AB,   2, kOut_1,     1 },  // Add
            { "Lerp",     IM_COL32( 90, 90, 90,255), kIn_ABT,  3, kOut_1,     1 },  // Lerp
            { "Remap",    IM_COL32( 90, 90, 90,255), kIn_In,   1, kOut_1,     1 },  // Remap
            { "Split",    IM_COL32( 90, 80,110,255), kIn_RGBA, 1, kOut_Split, 4 },  // Split
            { "Output",   IM_COL32(120, 70, 70,255), kIn_Out16,16, nullptr,   0 },  // Output (slot 6 = Surface layer; 7-10 = coat/aniso; 11-15 = transmission)
            { "Switch",   IM_COL32( 70,110,110,255), kIn_OffOn,2, kOut_1,     1 },  // StaticSwitch
            { "Subtract", IM_COL32( 90, 90, 90,255), kIn_AB,   2, kOut_1,     1 },  // Subtract
            { "Divide",   IM_COL32( 90, 90, 90,255), kIn_AB,   2, kOut_1,     1 },  // Divide
            { "Power",    IM_COL32( 90, 90, 90,255), kIn_AB,   2, kOut_1,     1 },  // Power
            { "Min",      IM_COL32( 90, 90, 90,255), kIn_AB,   2, kOut_1,     1 },  // Min
            { "Max",      IM_COL32( 90, 90, 90,255), kIn_AB,   2, kOut_1,     1 },  // Max
            { "Dot",      IM_COL32( 90, 90, 90,255), kIn_AB,   2, kOut_1,     1 },  // Dot
            { "Abs",      IM_COL32( 90, 90, 90,255), kIn_In,   1, kOut_1,     1 },  // Abs
            { "Saturate", IM_COL32( 90, 90, 90,255), kIn_In,   1, kOut_1,     1 },  // Saturate
            { "One Minus",IM_COL32( 90, 90, 90,255), kIn_In,   1, kOut_1,     1 },  // OneMinus
            { "UV",       IM_COL32( 70, 90,120,255), nullptr,  0, kOut_1,     1 },  // UV
            { "Noise",    IM_COL32( 70,120, 90,255), kIn_UV,   1, kOut_1,     1 },  // Noise
            { "World Pos",IM_COL32( 70, 90,120,255), nullptr,  0, kOut_1,     1 },  // WorldPos
            { "View Dir", IM_COL32( 70, 90,120,255), nullptr,  0, kOut_1,     1 },  // ViewDir
            { "Time",     IM_COL32( 70, 90,120,255), nullptr,  0, kOut_1,     1 },  // Time
            { "Fresnel",  IM_COL32(110, 90, 60,255), nullptr,  0, kOut_1,     1 },  // Fresnel
            { "Custom",   IM_COL32(110, 70,110,255), kIn_ABCD, 4, kOut_1,     1 },  // Custom
            { "Triplanar",    IM_COL32( 70,120, 90,255), nullptr,     0, kOut_1,     1 },  // Triplanar
            { "Detail Normal",IM_COL32( 80,110,110,255), kIn_BaseDet, 2, kOut_1,     1 },  // DetailNormal
            { "Make Layer",   IM_COL32( 90,120, 90,255), kIn_Layer15,15, kOut_Layer, 1 },  // MakeLayer (6 channels + 4 coat/aniso + 5 transmission)
            { "Layer Blend",  IM_COL32( 90,120, 90,255), kIn_BTM,     3, kOut_Layer, 1 },  // LayerBlend
            { "Parallax",     IM_COL32( 70,120,100,255), kIn_UV,      1, kOut_1,     1 },  // Parallax (UV pin optional)
            { "Decal",        IM_COL32(100,120, 80,255), kIn_BaseUV,  2, kOut_Layer, 1 },  // Decal (Base layer + optional UV)
        };
        constexpr size_t kTypeCount = sizeof(kTypes) / sizeof(kTypes[0]);

        // A "layer" pin carries a MaterialInputs bundle (green); a value pin a float4 (default). Used to
        // color pins and to reject cross-type links in AddLink, since the vendored AllowedLink lacks slots.
        const ImU32 kLayerPinCol = IM_COL32(120, 200, 120, 255);
        const ImU32 kValuePinCol = IM_COL32(170, 170, 170, 255);

        bool PinIsLayer(MatNodeType t, u8 slot, bool isOutput)
        {
            if (isOutput) return IsLayerNode(t);                  // MakeLayer / LayerBlend emit a layer
            if (t == MatNodeType::LayerBlend) return slot < 2;    // Bottom, Top are layers; Mask is a value
            if (t == MatNodeType::Output)     return slot == 6;   // the Surface pin
            if (t == MatNodeType::Decal)      return slot == 0;   // Base is a layer; UV is a value
            return false;
        }

        ImU32* InPinColors(MatNodeType t)   // GraphEditor::Template holds non-const ImU32* (read-only in practice)
        {
            static ImU32 layerBlend[] = { kLayerPinCol, kLayerPinCol, kValuePinCol };
            static ImU32 out16[] = { kValuePinCol, kValuePinCol, kValuePinCol, kValuePinCol, kValuePinCol,
                                     kValuePinCol, kLayerPinCol, kValuePinCol, kValuePinCol, kValuePinCol, kValuePinCol,
                                     kValuePinCol, kValuePinCol, kValuePinCol, kValuePinCol, kValuePinCol };
            static ImU32 decal[] = { kLayerPinCol, kValuePinCol };
            if (t == MatNodeType::LayerBlend) return layerBlend;
            if (t == MatNodeType::Decal)      return decal;
            if (t == MatNodeType::Output)     return out16;
            return nullptr;   // GraphEditor falls back to a default pin color
        }

        ImU32* OutPinColors(MatNodeType t)
        {
            static ImU32 layerOut[] = { kLayerPinCol };
            return IsLayerNode(t) ? layerOut : nullptr;
        }

        // A useful starter: baseColor = diffuse-texture x warm tint. Visible even with no diffuse map
        // (texture slot 0 is the reserved white texel), so creating it always tints the material.
        MaterialGraph DefaultGraph()
        {
            MaterialGraph g;
            g.nodes.push_back({ 1, MatNodeType::TextureSample, Vec4(0.0f), (u32)MapType::Diffuse, Vec2(40.0f,  90.0f) });
            g.nodes.push_back({ 2, MatNodeType::ConstColor,    Vec4(1.0f, 0.5f, 0.2f, 1.0f), 0,    Vec2(40.0f, 250.0f) });
            g.nodes.push_back({ 3, MatNodeType::Multiply,      Vec4(0.0f), 0,                       Vec2(280.0f,150.0f) });
            g.nodes.push_back({ 4, MatNodeType::Output,        Vec4(0.0f), 0,                       Vec2(520.0f,120.0f) });
            g.links.push_back({ 1, 0, 3, 0 });   // texture.out -> Multiply.A
            g.links.push_back({ 2, 0, 3, 1 });   // tint.out    -> Multiply.B
            g.links.push_back({ 3, 0, 4, 0 });   // Multiply.out-> Output.BaseColor
            return g;
        }

        u32 NextNodeId(const MaterialGraph& g)
        {
            u32 maxId = 0;
            for (const auto& n : g.nodes) maxId = std::max(maxId, n.id);
            return maxId + 1;
        }

        MatNode MakeNode(MatNodeType type, u32 id, Vec2 pos)
        {
            MatNode n;
            n.id = id; n.type = type; n.tex = 0; n.pos = pos;
            switch (type)
            {
                case MatNodeType::ConstFloat: n.value = Vec4(0.5f); break;
                case MatNodeType::ConstColor: n.value = Vec4(1.0f); break;
                case MatNodeType::Remap:      n.value = Vec4(0.0f, 1.0f, 0.0f, 1.0f); break;
                case MatNodeType::Noise:      n.value = Vec4(8.0f, 3.0f, 0.0f, 0.0f); break;   // (scale, octaves)
                case MatNodeType::Fresnel:    n.value = Vec4(5.0f, 0.0f, 0.0f, 0.0f); break;   // Schlick power
                case MatNodeType::Triplanar:    n.value = Vec4(4.0f, 0.0f, 0.0f, 0.0f); break; // tiling
                case MatNodeType::DetailNormal: n.value = Vec4(1.0f, 0.0f, 0.0f, 0.0f); break; // strength
                case MatNodeType::Parallax:     n.value = Vec4(0.05f, 8.0f, 32.0f, 0.0f); break; // height, min/max steps
                case MatNodeType::Decal:        n.value = Vec4(0.5f, 0.5f, 0.25f, 0.0f); break;  // center uv, scale, rotation
                default:                      n.value = Vec4(0.0f); break;
            }
            return n;
        }

        void DeleteNode(MaterialGraph& g, size_t index)
        {
            if (index >= g.nodes.size()) return;
            const u32 id = g.nodes[index].id;
            g.links.erase(std::remove_if(g.links.begin(), g.links.end(),
                [&](const MatLink& l) { return l.fromNode == id || l.toNode == id; }), g.links.end());
            g.nodes.erase(g.nodes.begin() + index);
        }

        // A settled node-param edit, split by downstream cost: a Const/Remap VALUE change is pure data (refresh
        // gMatParams, no recompile); a TextureSample slot change is STRUCTURE (re-emit the per-material shader);
        // exposed-parameter METADATA (name/group/ui) only needs saving.
        struct NodeEdit { bool value = false; bool structure = false; bool meta = false; };

        std::string Trimmed(const std::string& s)
        {
            const size_t b = s.find_first_not_of(" \t");
            const size_t e = s.find_last_not_of(" \t");
            return b == std::string::npos ? std::string{} : s.substr(b, e - b + 1);
        }

        // Name/group/checkbox fields that expose a value node as a named Inspector parameter. Metadata is
        // codegen-blind (never enters the canonical source), so naming can't split structure-shared shaders.
        void DrawExposeFields(const MaterialGraph& g, MatNode& n, NodeEdit& e)
        {
            UI::SeparatorText("Parameter");

            char nameBuf[64] = {};
            snprintf(nameBuf, sizeof(nameBuf), "%s", n.name.c_str());
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::InputTextWithHint("##pname", "expose as...", nameBuf, sizeof(nameBuf)))
                n.name = nameBuf;
            if (ImGui::IsItemDeactivatedAfterEdit()) { n.name = Trimmed(n.name); e.meta = true; }
            if (n.name.empty()) return;

            char groupBuf[64] = {};
            snprintf(groupBuf, sizeof(groupBuf), "%s", n.group.c_str());
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::InputTextWithHint("##pgroup", "group...", groupBuf, sizeof(groupBuf)))
                n.group = groupBuf;
            if (ImGui::IsItemDeactivatedAfterEdit()) { n.group = Trimmed(n.group); e.meta = true; }

            if (n.type == MatNodeType::ConstFloat)
            {
                bool cb = n.ui == 1;
                if (ImGui::Checkbox("Checkbox (0/1)", &cb)) { n.ui = cb ? 1 : 0; e.meta = true; }
            }

            // Advisory only: identity is the node id, so duplicate labels are harmless but confusing.
            for (const auto& other : g.nodes)
                if (other.id != n.id && !other.name.empty() && other.name == n.name)
                {
                    ImGui::TextDisabled("name also used by another node");
                    break;
                }
        }

        // Draws the selected node's editable parameters; flags settled edits (release / combo change). Const
        // values now flow to per-material data, so they no longer recompile; only structural edits re-emit.
        NodeEdit DrawNodeParams(const MaterialGraph& g, MatNode& n)
        {
            NodeEdit e;
            switch (n.type)
            {
                case MatNodeType::ConstFloat:
                    ImGui::DragFloat("Value", &n.value.x, 0.01f);
                    e.value = ImGui::IsItemDeactivatedAfterEdit();
                    break;
                case MatNodeType::ConstColor:
                    ImGui::ColorEdit4("Color", &n.value.x, ImGuiColorEditFlags_AlphaBar);
                    e.value = ImGui::IsItemDeactivatedAfterEdit();
                    break;
                case MatNodeType::Remap:
                    ImGui::DragFloat("In Min",  &n.value.x, 0.01f); e.value |= ImGui::IsItemDeactivatedAfterEdit();
                    ImGui::DragFloat("In Max",  &n.value.y, 0.01f); e.value |= ImGui::IsItemDeactivatedAfterEdit();
                    ImGui::DragFloat("Out Min", &n.value.z, 0.01f); e.value |= ImGui::IsItemDeactivatedAfterEdit();
                    ImGui::DragFloat("Out Max", &n.value.w, 0.01f); e.value |= ImGui::IsItemDeactivatedAfterEdit();
                    break;
                case MatNodeType::TextureSample:
                {
                    static const char* kMap[] = { "Diffuse","Alpha","Normal","Metallic","Roughness","Specular","Occlusion","Emissive","Thickness" };
                    int t = (n.tex < 9) ? (int)n.tex : 0;
                    if (ImGui::Combo("Map", &t, kMap, 9)) { n.tex = (u32)t; e.structure = true; }
                    break;
                }
                case MatNodeType::StaticSwitch:
                {
                    bool on = n.value.x != 0.0f;
                    // Compile-time state: flipping selects which branch is EMITTED, so it recompiles.
                    if (ImGui::Checkbox("On", &on)) { n.value.x = on ? 1.0f : 0.0f; e.structure = true; }
                    break;
                }
                case MatNodeType::UV:
                {
                    static const char* kSets[] = { "UV0", "UV1" };
                    int s = n.tex != 0u ? 1 : 0;
                    if (ImGui::Combo("Set", &s, kSets, 2)) { n.tex = (u32)s; e.structure = true; }
                    break;
                }
                case MatNodeType::Noise:
                    ImGui::DragFloat("Scale",   &n.value.x, 0.05f, 0.01f, 512.0f);
                    e.value |= ImGui::IsItemDeactivatedAfterEdit();
                    ImGui::DragFloat("Octaves", &n.value.y, 0.05f, 1.0f, 4.0f, "%.0f");
                    e.value |= ImGui::IsItemDeactivatedAfterEdit();
                    break;
                case MatNodeType::Fresnel:
                    ImGui::DragFloat("Power", &n.value.x, 0.05f, 0.1f, 32.0f);
                    e.value |= ImGui::IsItemDeactivatedAfterEdit();
                    break;
                case MatNodeType::Custom:
                {
                    ImGui::TextDisabled("float4 expression\nover a, b, c, d");
                    char code[1024] = {};
                    snprintf(code, sizeof(code), "%s", n.code.c_str());
                    if (ImGui::InputTextMultiline("##code", code, sizeof(code), ImVec2(-FLT_MIN, 80.0f)))
                        n.code = code;
                    if (ImGui::IsItemDeactivatedAfterEdit()) e.structure = true;   // code is emission -> recompile
                    if (const char* bad = MaterialGraphCodegen::ValidateCustomCode(n.code))
                        ImGui::TextColored(ImVec4(0.9f, 0.4f, 0.3f, 1.0f), "banned: %s", bad);
                    break;
                }
                case MatNodeType::Triplanar:
                {
                    static const char* kMap[] = { "Diffuse","Alpha","Normal","Metallic","Roughness","Specular","Occlusion","Emissive","Thickness" };
                    int t = (n.tex < 9) ? (int)n.tex : 0;
                    if (ImGui::Combo("Map", &t, kMap, 9)) { n.tex = (u32)t; e.structure = true; }
                    ImGui::DragFloat("Tiling", &n.value.x, 0.05f, 0.01f, 256.0f);
                    e.value |= ImGui::IsItemDeactivatedAfterEdit();
                    break;
                }
                case MatNodeType::DetailNormal:
                    ImGui::DragFloat("Strength", &n.value.x, 0.02f, 0.0f, 4.0f);
                    e.value |= ImGui::IsItemDeactivatedAfterEdit();
                    break;
                case MatNodeType::Parallax:
                    ImGui::DragFloat("Height",    &n.value.x, 0.002f, 0.0f, 0.5f, "%.3f");
                    e.value |= ImGui::IsItemDeactivatedAfterEdit();
                    ImGui::DragFloat("Min Steps", &n.value.y, 0.5f, 1.0f, 64.0f, "%.0f");
                    e.value |= ImGui::IsItemDeactivatedAfterEdit();
                    ImGui::DragFloat("Max Steps", &n.value.z, 0.5f, 1.0f, 64.0f, "%.0f");
                    e.value |= ImGui::IsItemDeactivatedAfterEdit();
                    break;
                case MatNodeType::Decal:
                {
                    ImGui::DragFloat("Offset X", &n.value.x, 0.005f);
                    e.value |= ImGui::IsItemDeactivatedAfterEdit();
                    ImGui::DragFloat("Offset Y", &n.value.y, 0.005f);
                    e.value |= ImGui::IsItemDeactivatedAfterEdit();
                    ImGui::DragFloat("Scale", &n.value.z, 0.005f, 0.001f, 8.0f, "%.3f");
                    e.value |= ImGui::IsItemDeactivatedAfterEdit();
                    float deg = n.value.w * (180.0f / Math::Pi<f32>);
                    if (ImGui::DragFloat("Rotation", &deg, 0.5f, -360.0f, 360.0f, "%.0f deg"))
                        n.value.w = deg * (Math::Pi<f32> / 180.0f);
                    e.value |= ImGui::IsItemDeactivatedAfterEdit();
                    break;
                }
                default:
                    ImGui::TextDisabled("No parameters.");
                    break;
            }
            if (IsExposableNode(n.type))
                DrawExposeFields(g, n, e);
            return e;
        }

        // Case-insensitive substring match for the node-search filter.
        bool NameMatches(const char* name, const char* filter)
        {
            if (!filter || !filter[0]) return true;
            std::string n = name, f = filter;
            std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c){ return (char)std::tolower(c); });
            std::transform(f.begin(), f.end(), f.begin(), [](unsigned char c){ return (char)std::tolower(c); });
            return n.find(f) != std::string::npos;
        }

        // Filtered node palette: a search box + the matching non-terminal types. Adds the picked type at
        // `pos` (Enter adds the first match). Returns true if a node was added.
        bool DrawNodePalette(MaterialGraph& g, char* filter, size_t filterCap, Vec2 pos)
        {
            ImGui::SetNextItemWidth(150.0f);
            ImGui::InputTextWithHint("##nodefilter", "search...", filter, filterCap);
            bool added = false;
            int  firstMatch = -1;
            for (int t = 0; t < (int)kTypeCount; ++t)
            {
                if ((MatNodeType)t == MatNodeType::Output) continue;
                if (!NameMatches(kTypes[t].name, filter)) continue;
                if (firstMatch < 0) firstMatch = t;
                if (ImGui::MenuItem(kTypes[t].name))
                {
                    g.nodes.push_back(MakeNode((MatNodeType)t, NextNodeId(g), pos));
                    added = true;
                }
            }
            if (firstMatch >= 0 && ImGui::IsKeyPressed(ImGuiKey_Enter))
            {
                g.nodes.push_back(MakeNode((MatNodeType)firstMatch, NextNodeId(g), pos));
                added = true;
            }
            return added;
        }
    }

    // ---- GraphDelegate ----

    void MaterialGraphPanel::GraphDelegate::Bind(MaterialGraph* g)
    {
        graph = g;
        idToIndex.clear();
        indexToId.clear();
        const size_t count = g ? g->nodes.size() : 0;
        indexToId.reserve(count);
        for (size_t i = 0; i < count; ++i)
        {
            idToIndex[g->nodes[i].id] = i;
            indexToId.push_back(g->nodes[i].id);
        }
        if (selected.size() != count) selected.assign(count, false);
    }

    bool MaterialGraphPanel::GraphDelegate::AllowedLink(GraphEditor::NodeIndex from, GraphEditor::NodeIndex to)
    {
        return from != to;   // GraphEditor enforces output->input pin sides; just block self-links
    }

    void MaterialGraphPanel::GraphDelegate::SelectNode(GraphEditor::NodeIndex nodeIndex, bool sel)
    {
        if (nodeIndex < selected.size()) selected[nodeIndex] = sel;
    }

    void MaterialGraphPanel::GraphDelegate::MoveSelectedNodes(const ImVec2 delta)
    {
        if (!graph) return;
        for (size_t i = 0; i < selected.size() && i < graph->nodes.size(); ++i)
            if (selected[i]) { graph->nodes[i].pos.x += delta.x; graph->nodes[i].pos.y += delta.y; moved = true; }
    }

    void MaterialGraphPanel::GraphDelegate::AddLink(GraphEditor::NodeIndex inNode, GraphEditor::SlotIndex inSlot,
                                                    GraphEditor::NodeIndex outNode, GraphEditor::SlotIndex outSlot)
    {
        if (!graph || inNode >= indexToId.size() || outNode >= indexToId.size()) return;
        const u32 fromId = indexToId[inNode];   // producer (output pin)
        const u32 toId   = indexToId[outNode];  // consumer (input pin)

        // Type-check the wire: the vendored AllowedLink sees only node indices, so the float4-vs-layer
        // guard lives here where the slot indices are known. Reject a cross-type connection.
        if (PinIsLayer(graph->nodes[inNode].type, (u8)inSlot, true)
            != PinIsLayer(graph->nodes[outNode].type, (u8)outSlot, false))
            return;

        // Single-input semantics: a consumer input slot holds at most one link.
        graph->links.erase(std::remove_if(graph->links.begin(), graph->links.end(),
            [&](const MatLink& l) { return l.toNode == toId && l.toSlot == (u8)outSlot; }), graph->links.end());

        MatLink l;
        l.fromNode = fromId; l.fromSlot = (u8)inSlot;
        l.toNode   = toId;   l.toSlot   = (u8)outSlot;
        graph->links.push_back(l);
        structuralChange = true;
    }

    void MaterialGraphPanel::GraphDelegate::DelLink(GraphEditor::LinkIndex linkIndex)
    {
        if (!graph || linkIndex >= graph->links.size()) return;
        graph->links.erase(graph->links.begin() + linkIndex);
        structuralChange = true;
    }

    void MaterialGraphPanel::GraphDelegate::CustomDraw(ImDrawList* drawList, ImRect rect, GraphEditor::NodeIndex nodeIndex)
    {
        if (!graph || nodeIndex >= graph->nodes.size()) return;
        const MatNode& n = graph->nodes[nodeIndex];
        char buf[80] = {};
        switch (n.type)
        {
            case MatNodeType::ConstFloat: snprintf(buf, sizeof(buf), "%.3g", n.value.x); break;
            case MatNodeType::ConstColor: snprintf(buf, sizeof(buf), "%.2f %.2f %.2f", n.value.x, n.value.y, n.value.z); break;
            case MatNodeType::Remap:      snprintf(buf, sizeof(buf), "[%.2g %.2g]->[%.2g %.2g]", n.value.x, n.value.y, n.value.z, n.value.w); break;
            case MatNodeType::TextureSample:
            {
                static const char* kMap[] = { "Diffuse","Alpha","Normal","Metallic","Roughness","Specular","Occlusion","Emissive","Thickness" };
                snprintf(buf, sizeof(buf), "%s", kMap[n.tex < 9 ? n.tex : 0]);
                break;
            }
            case MatNodeType::StaticSwitch: snprintf(buf, sizeof(buf), "%s", n.value.x != 0.0f ? "On" : "Off"); break;
            case MatNodeType::UV:           snprintf(buf, sizeof(buf), "UV%u", n.tex != 0u ? 1u : 0u); break;
            case MatNodeType::Noise:        snprintf(buf, sizeof(buf), "x%.3g o%d", n.value.x, (int)n.value.y); break;
            case MatNodeType::Fresnel:      snprintf(buf, sizeof(buf), "p%.3g", n.value.x); break;
            case MatNodeType::Triplanar:
            {
                static const char* kMap[] = { "Diffuse","Alpha","Normal","Metallic","Roughness","Specular","Occlusion","Emissive","Thickness" };
                snprintf(buf, sizeof(buf), "%s x%.2g", kMap[n.tex < 9 ? n.tex : 0], n.value.x);
                break;
            }
            case MatNodeType::DetailNormal: snprintf(buf, sizeof(buf), "s%.2g", n.value.x); break;
            case MatNodeType::Parallax:     snprintf(buf, sizeof(buf), "h%.3g", n.value.x); break;
            case MatNodeType::Decal:        snprintf(buf, sizeof(buf), "(%.2g,%.2g) x%.2g", n.value.x, n.value.y, n.value.z); break;
            default: return;
        }
        drawList->AddText(ImVec2(rect.Min.x + 4.0f, rect.Min.y + 2.0f), IM_COL32(210, 210, 210, 255), buf);
    }

    void MaterialGraphPanel::GraphDelegate::RightClick(GraphEditor::NodeIndex nodeIndex, GraphEditor::SlotIndex, GraphEditor::SlotIndex)
    {
        rightClickedNode = nodeIndex;   // the panel opens the add/delete popup after Show
        openMenu = true;
    }

    const size_t MaterialGraphPanel::GraphDelegate::GetTemplateCount() { return kTypeCount; }

    const GraphEditor::Template MaterialGraphPanel::GraphDelegate::GetTemplate(GraphEditor::TemplateIndex index)
    {
        const size_t ti = index < kTypeCount ? index : 0;
        const TypeInfo& t = kTypes[ti];
        const MatNodeType nt = (MatNodeType)ti;
        GraphEditor::Template tpl{};
        tpl.mHeaderColor         = t.header;
        tpl.mBackgroundColor     = IM_COL32(50, 50, 50, 255);
        tpl.mBackgroundColorOver = IM_COL32(64, 64, 64, 255);
        tpl.mInputCount  = t.inCount;
        tpl.mInputNames  = t.inNames;
        tpl.mInputColors = InPinColors(nt);
        tpl.mOutputCount = t.outCount;
        tpl.mOutputNames = t.outNames;
        tpl.mOutputColors= OutPinColors(nt);
        return tpl;
    }

    const size_t MaterialGraphPanel::GraphDelegate::GetNodeCount() { return graph ? graph->nodes.size() : 0; }

    const GraphEditor::Node MaterialGraphPanel::GraphDelegate::GetNode(GraphEditor::NodeIndex index)
    {
        const MatNode& n = graph->nodes[index];
        const TypeInfo& t = kTypes[(int)n.type < (int)kTypeCount ? (int)n.type : 0];
        const u8 pins = std::max(t.inCount, t.outCount);
        const float w = 150.0f;
        const float h = 34.0f + pins * 20.0f;

        GraphEditor::Node node;
        // Exposed nodes are titled by their parameter name (string stays alive on the bound graph).
        node.mName          = n.name.empty() ? t.name : n.name.c_str();
        node.mTemplateIndex = (GraphEditor::TemplateIndex)n.type;
        node.mRect          = ImRect(ImVec2(n.pos.x, n.pos.y), ImVec2(n.pos.x + w, n.pos.y + h));
        node.mSelected      = index < selected.size() ? selected[index] : false;
        return node;
    }

    const size_t MaterialGraphPanel::GraphDelegate::GetLinkCount() { return graph ? graph->links.size() : 0; }

    const GraphEditor::Link MaterialGraphPanel::GraphDelegate::GetLink(GraphEditor::LinkIndex index)
    {
        const MatLink& l = graph->links[index];
        auto idx = [&](u32 id) -> GraphEditor::NodeIndex {
            auto it = idToIndex.find(id);
            return it == idToIndex.end() ? 0 : it->second;
        };
        GraphEditor::Link link;
        link.mInputNodeIndex  = idx(l.fromNode);   // producer's OUTPUT pin
        link.mInputSlotIndex  = l.fromSlot;
        link.mOutputNodeIndex = idx(l.toNode);     // consumer's INPUT pin
        link.mOutputSlotIndex = l.toSlot;
        return link;
    }

    // ---- Panel ----

    MaterialGraphPanel::MaterialGraphPanel() { m_WindowID = "MaterialGraph"; }

    void MaterialGraphPanel::OnDraw(const EditorSnapshot& /*snapshot*/)
    {
        LH_PROFILE_FUNCTION();
        // ImGui::Begin must always pair with End(), even on early-out or a throw. The guard keeps the
        // window stack balanced so a mid-draw exception is logged by the panel error boundary instead of
        // tripping ImGui's "Missing End()" assert on the next frame.
        const bool open = BeginWindow(ICON_NODE_GRAPH "  Material Graph");
        struct EndGuard { ~EndGuard() { ImGui::End(); } } endGuard;
        if (!open) return;

        // Type-check the selection: GetAsset<Material> does not validate the asset type, so a non-material
        // selection (a model, texture, ...) would otherwise reinterpret foreign memory as a Material.
        const UUID sel = EditorSelection::GetSelectedResource();
        std::shared_ptr<Material> material;
        if (sel.IsValid() && AssetDatabase::GetMetadata(sel).Type == AssetType::Material)
            material = AssetManager::GetAsset<Material>(sel);

        if (!material)
        {
            ImGui::TextDisabled("Select a material to edit its node graph.");
            return;
        }

        if (sel != m_BoundMaterial)
        {
            m_BoundMaterial = sel;
            m_Delegate.selected.clear();
            m_View = GraphEditor::ViewState{};
        }

        ImGui::TextDisabled("%s", material->GetName().c_str());

        if (!material->HasGraph())
        {
            ImGui::Separator();
            ImGui::TextWrapped("This material renders with the stock PBR shader. Create a node graph "
                               "to route the material's channels (baseColor / metallic / roughness / AO / emissive).");
            if (ImGui::Button("Create Material Graph"))
            {
                material->SetGraph(DefaultGraph());
                material->MarkDirty();
                MaterialGraphCodegen::GenerateAndCompile(*material);
                m_Delegate.selected.clear();
            }
            return;
        }

        ImGui::SameLine();
        if (ImGui::SmallButton("Recompile")) MaterialGraphCodegen::GenerateAndCompile(*material);

        MaterialGraph& graph = material->GetGraphMutable();
        bool recodegen = false;   // structural edit -> re-emit the shader
        bool valueEdit = false;   // Const/Remap value -> per-material data only (no recompile)
        bool metaEdit  = false;   // expose name/group/ui -> save only

        // Left: parameters for the selected node. Right: the graph canvas.
        ImGui::BeginChild("##ParamPane", ImVec2(210.0f, 0.0f), true);
        {
            int selNode = -1;
            for (size_t i = 0; i < m_Delegate.selected.size(); ++i)
                if (m_Delegate.selected[i]) { selNode = (int)i; break; }

            if (selNode >= 0 && selNode < (int)graph.nodes.size())
            {
                MatNode& n = graph.nodes[selNode];
                ImGui::TextUnformatted(kTypes[(int)n.type].name);
                ImGui::Separator();
                NodeEdit edit = DrawNodeParams(graph, n);
                if (edit.structure) recodegen = true;
                if (edit.value)     valueEdit = true;
                if (edit.meta)      metaEdit  = true;
                if (n.type != MatNodeType::Output)
                {
                    ImGui::Separator();
                    if (ImGui::Button("Delete Node"))
                    {
                        DeleteNode(graph, (size_t)selNode);
                        m_Delegate.selected.clear();
                        recodegen = true;
                    }
                }
            }
            else
            {
                ImGui::TextDisabled("Select a node to edit it,\nor right-click the canvas\nto add one.");
            }
        }
        ImGui::EndChild();
        ImGui::SameLine();

        // GraphEditor draws into the remaining window region (matches the working direct-call site).
        m_Delegate.Bind(&graph);
        GraphEditor::Show(m_Delegate, m_Options, m_View, true, nullptr);

        if (m_Delegate.openMenu) { ImGui::OpenPopup("##GraphCtx"); m_Delegate.openMenu = false; }
        if (ImGui::BeginPopup("##GraphCtx"))
        {
            if (ImGui::BeginMenu("Add Node"))
            {
                for (int t = 0; t < (int)kTypeCount; ++t)
                {
                    if ((MatNodeType)t == MatNodeType::Output) continue;   // single terminal, ships with the graph
                    if (ImGui::MenuItem(kTypes[t].name))
                    {
                        const float c = (graph.nodes.size() % 6) * 26.0f;
                        graph.nodes.push_back(MakeNode((MatNodeType)t, NextNodeId(graph), Vec2(60.0f + c, 60.0f + c)));
                        recodegen = true;
                    }
                }
                ImGui::EndMenu();
            }
            if (m_Delegate.rightClickedNode < graph.nodes.size()
                && graph.nodes[m_Delegate.rightClickedNode].type != MatNodeType::Output)
            {
                ImGui::Separator();
                if (ImGui::MenuItem("Delete Node"))
                {
                    DeleteNode(graph, m_Delegate.rightClickedNode);
                    m_Delegate.selected.clear();
                    recodegen = true;
                }
            }
            ImGui::EndPopup();
        }

        // Space over the canvas opens a searchable quick-add (type to filter, Enter adds the first match).
        static char s_NodeFilter[32] = {};
        if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) && !ImGui::IsAnyItemActive()
            && ImGui::IsKeyPressed(ImGuiKey_Space))
        {
            s_NodeFilter[0] = '\0';
            ImGui::OpenPopup("##QuickAdd");
        }
        if (ImGui::BeginPopup("##QuickAdd"))
        {
            if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
            const float c = (graph.nodes.size() % 6) * 26.0f;
            if (DrawNodePalette(graph, s_NodeFilter, sizeof(s_NodeFilter), Vec2(60.0f + c, 60.0f + c)))
            {
                recodegen = true;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        if (m_Delegate.structuralChange) { recodegen = true; m_Delegate.structuralChange = false; }
        if (m_Delegate.moved)            { material->MarkDirty(); m_Delegate.moved = false; }
        if (recodegen)
        {
            MaterialGraphCodegen::GenerateAndCompile(*material);
            material->MarkDirty();
        }
        else if (valueEdit)
        {
            // Value-only edit: lower the constants into per-material data; the generated shader is untouched.
            MaterialGraphCodegen::RefreshParams(*material);
            material->MarkDirty();
        }
        else if (metaEdit)
        {
            material->MarkDirty();   // expose metadata is structure-neutral; just persist it
        }
    }
}
