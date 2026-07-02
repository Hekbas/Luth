#pragma once

#include "luthien/Editor.h"
#include "luth/core/UUID.h"
#include "luth/renderer/material/MaterialGraph.h"

#include <GraphEditor.h>

#include <unordered_map>
#include <vector>

namespace Luth
{
    // Node-based material editor: renders the selected material's MaterialGraph on an ImGuizmo GraphEditor
    // canvas and reflects edits back. Structural edits re-emit the material's fragment shader via
    // MaterialGraphCodegen (live in the viewport); Const/Remap values flow to per-material data with no
    // recompile. The left pane edits the selected node and can expose it as a named Inspector parameter.
    class MaterialGraphPanel : public Panel
    {
    public:
        MaterialGraphPanel();
        void OnDraw(const EditorSnapshot& snapshot) override;

    private:
        // Bridges a Material's graph to the GraphEditor canvas. Rebound to the active material each frame;
        // node-selection state persists across rebinds. Links use GraphEditor's convention: the "input"
        // end is the producer's OUTPUT pin, the "output" end is the consumer's INPUT pin.
        struct GraphDelegate : public GraphEditor::Delegate
        {
            MaterialGraph* graph = nullptr;
            std::unordered_map<u32, size_t> idToIndex;   // MatNode.id -> canvas node index
            std::vector<u32>  indexToId;
            std::vector<bool> selected;
            bool structuralChange = false;   // link add/remove -> re-codegen
            bool moved = false;              // node drag -> save (position only)
            bool openMenu = false;           // right-click -> open the canvas context menu
            GraphEditor::NodeIndex rightClickedNode = 0;

            void Bind(MaterialGraph* g);

            bool AllowedLink(GraphEditor::NodeIndex from, GraphEditor::NodeIndex to) override;
            void SelectNode(GraphEditor::NodeIndex nodeIndex, bool sel) override;
            void MoveSelectedNodes(const ImVec2 delta) override;
            void AddLink(GraphEditor::NodeIndex inNode, GraphEditor::SlotIndex inSlot,
                         GraphEditor::NodeIndex outNode, GraphEditor::SlotIndex outSlot) override;
            void DelLink(GraphEditor::LinkIndex linkIndex) override;
            void CustomDraw(ImDrawList* drawList, ImRect rect, GraphEditor::NodeIndex nodeIndex) override;
            void RightClick(GraphEditor::NodeIndex nodeIndex, GraphEditor::SlotIndex slotIn,
                            GraphEditor::SlotIndex slotOut) override;
            const size_t GetTemplateCount() override;
            const GraphEditor::Template GetTemplate(GraphEditor::TemplateIndex index) override;
            const size_t GetNodeCount() override;
            const GraphEditor::Node GetNode(GraphEditor::NodeIndex index) override;
            const size_t GetLinkCount() override;
            const GraphEditor::Link GetLink(GraphEditor::LinkIndex index) override;
        };

        GraphDelegate          m_Delegate;
        GraphEditor::Options   m_Options;
        GraphEditor::ViewState m_View;
        UUID                   m_BoundMaterial = UUID::Invalid();
    };
}
