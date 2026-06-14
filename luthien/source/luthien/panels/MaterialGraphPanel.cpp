#include "lepch.h"
#include "luthien/panels/MaterialGraphPanel.h"
#include "luthien/EditorSelection.h"
#include "luth/renderer/material/Material.h"
#include "luth/renderer/material/MaterialGraphCodegen.h"
#include "luth/resources/AssetManager.h"
#include "luth/resources/AssetDatabase.h"

#include <algorithm>
#include <cstdio>

namespace Luth
{
    namespace
    {
        // Per-node-type pin layout + label. Indexed by MatNodeType. Pin-name arrays are static so the
        // GraphEditor::Template's const char** pointers stay valid for the canvas's lifetime.
        const char* kIn_AB[]    = { "A", "B" };
        const char* kIn_ABT[]   = { "A", "B", "T" };
        const char* kIn_In[]    = { "in" };
        const char* kIn_RGBA[]  = { "rgba" };
        const char* kIn_Out[]   = { "BaseColor", "Metallic", "Roughness", "Normal", "AO", "Emissive" };
        const char* kOut_1[]    = { "out" };
        const char* kOut_RGBA[] = { "rgba" };
        const char* kOut_Split[]= { "R", "G", "B", "A" };

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
            { "Texture",  IM_COL32( 70,120, 90,255), nullptr,  0, kOut_RGBA,  1 },  // TextureSample
            { "Multiply", IM_COL32( 90, 90, 90,255), kIn_AB,   2, kOut_1,     1 },  // Multiply
            { "Add",      IM_COL32( 90, 90, 90,255), kIn_AB,   2, kOut_1,     1 },  // Add
            { "Lerp",     IM_COL32( 90, 90, 90,255), kIn_ABT,  3, kOut_1,     1 },  // Lerp
            { "Remap",    IM_COL32( 90, 90, 90,255), kIn_In,   1, kOut_1,     1 },  // Remap
            { "Split",    IM_COL32( 90, 80,110,255), kIn_RGBA, 1, kOut_Split, 4 },  // Split
            { "Output",   IM_COL32(120, 70, 70,255), kIn_Out,  6, nullptr,    0 },  // Output
        };
        constexpr size_t kTypeCount = sizeof(kTypes) / sizeof(kTypes[0]);

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
    }

    // ── GraphDelegate ──────────────────────────────────────────────────────────────────────────────

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
            default: return;
        }
        drawList->AddText(ImVec2(rect.Min.x + 4.0f, rect.Min.y + 2.0f), IM_COL32(210, 210, 210, 255), buf);
    }

    void MaterialGraphPanel::GraphDelegate::RightClick(GraphEditor::NodeIndex, GraphEditor::SlotIndex, GraphEditor::SlotIndex)
    {
        // Node-add palette / per-node context menu lands in the interactive-authoring follow-up.
    }

    const size_t MaterialGraphPanel::GraphDelegate::GetTemplateCount() { return kTypeCount; }

    const GraphEditor::Template MaterialGraphPanel::GraphDelegate::GetTemplate(GraphEditor::TemplateIndex index)
    {
        const TypeInfo& t = kTypes[index < kTypeCount ? index : 0];
        GraphEditor::Template tpl{};
        tpl.mHeaderColor         = t.header;
        tpl.mBackgroundColor     = IM_COL32(50, 50, 50, 255);
        tpl.mBackgroundColorOver = IM_COL32(64, 64, 64, 255);
        tpl.mInputCount  = t.inCount;
        tpl.mInputNames  = t.inNames;
        tpl.mInputColors = nullptr;
        tpl.mOutputCount = t.outCount;
        tpl.mOutputNames = t.outNames;
        tpl.mOutputColors= nullptr;
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
        node.mName          = t.name;
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

    // ── Panel ──────────────────────────────────────────────────────────────────────────────────────

    MaterialGraphPanel::MaterialGraphPanel() { m_WindowID = "MaterialGraph"; }

    void MaterialGraphPanel::OnDraw(const EditorSnapshot& /*snapshot*/)
    {
        // ImGui::Begin must always pair with End() — even on early-out or a throw. The guard keeps the
        // window stack balanced so a mid-draw exception is logged by the panel error boundary instead of
        // tripping ImGui's "Missing End()" assert on the next frame.
        const bool open = BeginWindow("Material Graph");
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

        m_Delegate.Bind(&material->GetGraphMutable());
        GraphEditor::Show(m_Delegate, m_Options, m_View, true, nullptr);

        if (m_Delegate.structuralChange)
        {
            MaterialGraphCodegen::GenerateAndCompile(*material);
            material->MarkDirty();
            m_Delegate.structuralChange = false;
        }
        if (m_Delegate.moved) { material->MarkDirty(); m_Delegate.moved = false; }
    }
}
