#include "luthpch.h"
#include "luth/editor/panels/HistoryPanel.h"
#include "luth/editor/CommandHistory.h"
#include "luth/editor/Command.h"
#include "luth/editor/widgets/Icons.h"
#include "luth/editor/EditorColors.h"
#include "luth/editor/Editor.h"

#include <imgui.h>
#include <imgui_internal.h>

namespace Luth
{
    static const char* GetCommandIcon(ICommand* cmd)
    {
        if (dynamic_cast<EntityCreateCommand*>(cmd))    return ICON_FA_PLUS;
        if (dynamic_cast<EntityDestroyCommand*>(cmd))   return ICON_FA_TRASH;
        if (dynamic_cast<EntityRenameCommand*>(cmd))    return ICON_FA_I;
        if (dynamic_cast<EntityReparentCommand*>(cmd))  return ICON_FA_ARROW_RIGHT_TO_BRACKET;
        if (dynamic_cast<EntityReorderCommand*>(cmd))   return ICON_FA_ARROWS_UP_DOWN;
        if (dynamic_cast<EntityDuplicateCommand*>(cmd)) return ICON_FA_CLONE;
        
        if (dynamic_cast<GizmoTransformCommand*>(cmd))   return ICON_FA_ARROWS_UP_DOWN_LEFT_RIGHT;
        if (dynamic_cast<MaterialSnapshotCommand*>(cmd)) return ICON_FA_IMAGE;
        if (dynamic_cast<ModelInstantiateCommand*>(cmd)) return ICON_FA_CUBES;
        
        if (dynamic_cast<CompoundCommand*>(cmd)) return ICON_FA_LAYER_GROUP;

        std::string name = cmd->GetName();
        if (name.find("Add ") != std::string::npos)     return ICON_FA_PLUS;
        if (name.find("Remove ") != std::string::npos)  return ICON_FA_MINUS;

        return ICON_FA_GEAR;
    }

    struct TimelineNodeInfo
    {
        bool IsFirst = false;
        bool IsLast = false;
        bool IsPast = false;
        bool IsPresent = false;
        bool IsFuture = false;
    };

    static void DrawTimelineNode(const TimelineNodeInfo& node)
    {
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        ImGuiContext& g = *GImGui;
        ImGuiTable* table = g.CurrentTable;
        if (!table) return;

        float cellPadY = g.Style.CellPadding.y;
        float rowHeight = ImGui::GetTextLineHeight() + cellPadY * 2.0f;
        
        float y_top = table->RowPosY1;
        float y_bot = y_top + rowHeight;
        float y_mid = y_top + rowHeight * 0.5f;
        float x_mid = table->Columns[0].MinX + 16.0f; // Center of the 32px column
        
        ImU32 colPast   = ImGui::GetColorU32(EditorColors::SuccessGreen);
        ImU32 colFuture = IM_COL32(100, 100, 100, 255);
        
        float thick = 3.0f;
        float thin = 1.0f;

        ImU32 colUpper = colPast;
        float thUpper = thick;
        ImU32 colLower = colPast;
        float thLower = thick;

        if (node.IsFuture)
        {
            colUpper = colFuture; thUpper = thin;
            colLower = colFuture; thLower = thin;
        }
        else if (node.IsPresent)
        {
            colUpper = colPast; thUpper = thick;
            colLower = colFuture; thLower = thin;
        }
        else if (node.IsPast)
        {
            colUpper = colPast; thUpper = thick;
            colLower = colPast; thLower = thick;
        }

        if (!node.IsFirst)
            drawList->AddLine(ImVec2(x_mid, y_top), ImVec2(x_mid, y_mid), colUpper, thUpper);
            
        if (!node.IsLast)
            drawList->AddLine(ImVec2(x_mid, y_mid), ImVec2(x_mid, y_bot), colLower, thLower);
    }

    HistoryPanel::HistoryPanel()
    {
        LH_CORE_INFO("Created History panel");
    }

    void HistoryPanel::OnInit() {}

    void HistoryPanel::OnRender()
    {
        if (ImGui::Begin(ICON_FA_CLOCK_ROTATE_LEFT "  History"))
        {
            auto& undoStack = CommandHistory::GetUndoStack();
            auto& redoStack = CommandHistory::GetRedoStack();

            // ── Header bar ──────────────────────────────────────
            {
                ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.12f, 0.12f, 0.12f, 1.0f));
                if (ImGui::BeginChild("HistoryHeader", ImVec2(0, 40), false, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse))
                {
                    ImGui::SetCursorPos(ImVec2(8, 8)); // Padding

                    bool canUndo = CommandHistory::CanUndo();
                    bool canRedo = CommandHistory::CanRedo();

                    ImGui::PushFont(Editor::GetFASolid());
                    
                    if (!canUndo) ImGui::BeginDisabled();
                    if (ImGui::Button(ICON_FA_ARROW_ROTATE_LEFT, ImVec2(32, 24)))
                        CommandHistory::Undo();
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) 
                        ImGui::SetTooltip("Undo (Ctrl+Z)");
                    if (!canUndo) ImGui::EndDisabled();

                    ImGui::SameLine();

                    if (!canRedo) ImGui::BeginDisabled();
                    if (ImGui::Button(ICON_FA_ARROW_ROTATE_RIGHT, ImVec2(32, 24)))
                        CommandHistory::Redo();
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) 
                        ImGui::SetTooltip("Redo (Ctrl+Y)");
                    if (!canRedo) ImGui::EndDisabled();

                    // Clear button right aligned
                    bool empty = undoStack.empty() && redoStack.empty();
                    float clearWidth = 32.0f;
                    ImGui::SameLine(ImGui::GetWindowWidth() - clearWidth - 8.0f);
                    if (empty) ImGui::BeginDisabled();
                    if (ImGui::Button(ICON_FA_TRASH, ImVec2(clearWidth, 24)))
                        CommandHistory::Clear();
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) 
                        ImGui::SetTooltip("Clear History");
                    if (empty) ImGui::EndDisabled();

                    ImGui::PopFont();
                }
                ImGui::EndChild();
                ImGui::PopStyleColor();

                // Status text
                ImGui::SetCursorPosX(8.0f);
                ImGui::TextDisabled("%zu Undo | %zu Redo", undoStack.size(), redoStack.size());
                
                if (CommandHistory::IsInCompound())
                {
                     ImGui::SameLine();
                     ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "| Recording: %s...", CommandHistory::GetCompoundName());
                }
            }
            ImGui::Separator();

            // ── Stack list ──────────────────────────────────────
            if (ImGui::BeginChild("HistoryList", ImVec2(0, 0), false))
            {
                constexpr ImGuiTableFlags tableFlags =
                    ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY;

                if (ImGui::BeginTable("HistoryTable", 3, tableFlags))
                {
                    ImGui::TableSetupColumn("Timeline", ImGuiTableColumnFlags_WidthFixed, 20.0f);
                    ImGui::TableSetupColumn("Command", ImGuiTableColumnFlags_WidthStretch);
                    ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 30.0f);

                    bool hasNodes = !undoStack.empty() || !redoStack.empty();
                    size_t totalNodes = hasNodes ? (undoStack.size() + redoStack.size()) : 1;
                    size_t currentNodeIndex = 0;

                    if (!hasNodes)
                    {
                        // --- 1. Initial State ---
                        ImGui::TableNextRow();
                        TimelineNodeInfo baseNode;
                        baseNode.IsFirst = true;
                        baseNode.IsLast  = (currentNodeIndex == totalNodes - 1);
                        baseNode.IsPast  = true;
                        baseNode.IsPresent = undoStack.empty();
                        
                        ImGui::TableSetColumnIndex(1);
                        ImVec2 baseCol1Cursor = ImGui::GetCursorScreenPos();
                        
                        if (undoStack.empty())
                        {
                            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.3f, 1.0f, 0.8f, 1.0f));
                            ImU32 bgCol = ImGui::GetColorU32(ImVec4(0.1f, 0.3f, 0.4f, 0.6f));
                            ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, bgCol);
                        }
                        else
                        {
                            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.6f, 0.6f, 1.0f));
                        }

                        ImGui::PushFont(Editor::GetFASolid());
                        ImGui::TextUnformatted(ICON_FA_CLOCK_ROTATE_LEFT);
                        ImGui::PopFont();
                        
                        ImGui::SameLine();
                        ImGui::SetCursorScreenPos(ImVec2(baseCol1Cursor.x + 26.0f, ImGui::GetCursorScreenPos().y));
                        ImGui::TextUnformatted("* But nobody came.");
                        ImGui::PopStyleColor();
                        
                        // Timeline Column
                        ImGui::TableSetColumnIndex(0);
                        ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, baseCol1Cursor.y));
                        DrawTimelineNode(baseNode);
                        
                        currentNodeIndex++;
                    }

                    auto renderCommands = [&](const std::vector<std::unique_ptr<ICommand>>& stack, bool isUndo, bool reverse) {
                        
                        i32 start = reverse ? (i32)stack.size() - 1 : 0;
                        i32 end   = reverse ? -1 : (i32)stack.size();
                        i32 step  = reverse ? -1 : 1;

                        for (i32 i = start; i != end; i += step)
                        {
                            auto& cmd = stack[i];
                            auto* compound = dynamic_cast<CompoundCommand*>(cmd.get());
                            
                            bool isCurrent = isUndo && (i == (i32)stack.size() - 1);
                            
                            ImGui::TableNextRow();
                            
                            // COL 1: Command
                            ImGui::TableSetColumnIndex(1);
                            ImVec2 col1Cursor = ImGui::GetCursorScreenPos();
                            
                            if (isCurrent)
                            {
                                ImU32 bgCol = ImGui::GetColorU32(ImVec4(0.1f, 0.3f, 0.4f, 0.6f));
                                ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, bgCol);
                            }
                            
                            if (isUndo)
                                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
                            else
                                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.6f, 0.6f, 1.0f));

                            const char* icon = GetCommandIcon(cmd.get());
                            
                            ImGui::PushFont(Editor::GetFASolid());
                            ImGui::TextUnformatted(icon);
                            ImGui::PopFont();
                            ImGui::SameLine();
                            ImGui::SetCursorScreenPos(ImVec2(col1Cursor.x + 26.0f, ImGui::GetCursorScreenPos().y));
                            
                            if (isCurrent)
                                ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.8f, 1.0f), "%s", cmd->GetName());
                            else
                                ImGui::TextUnformatted(cmd->GetName());
                                
                            ImGui::PopStyleColor();

                            // COL 2: Children Count
                            ImGui::TableSetColumnIndex(2);
                            if (compound)
                            {
                                ImGui::TextDisabled("%zu", compound->GetChildCount());
                                if (ImGui::IsItemHovered())
                                {
                                    ImGui::BeginTooltip();
                                    ImGui::TextDisabled("Commands:");
                                    ImGui::Separator();
                                    for (auto& child : compound->GetChildren())
                                    {
                                        ImGui::TextUnformatted(child->GetName());
                                    }
                                    ImGui::EndTooltip();
                                }
                            }
                            
                            // COL 0: Timeline
                            ImGui::TableSetColumnIndex(0);
                            TimelineNodeInfo nodeInfo;
                            nodeInfo.IsFirst = (currentNodeIndex == 0);
                            nodeInfo.IsLast  = (currentNodeIndex == totalNodes - 1);
                            nodeInfo.IsPast  = isUndo;
                            nodeInfo.IsFuture = !isUndo;
                            nodeInfo.IsPresent = isCurrent;
                            
                            ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, col1Cursor.y));
                            DrawTimelineNode(nodeInfo);
                            
                            currentNodeIndex++;
                        }
                    };

                    // 2. Past (Undo Stack: oldest to newest)
                    renderCommands(undoStack, true, false);

                    // 3. Future (Redo Stack: nearest to furthest -> reverse order)
                    renderCommands(redoStack, false, true);

                    ImGui::EndTable();
                }
            }
            ImGui::EndChild();
        }
        ImGui::End();
    }
}
