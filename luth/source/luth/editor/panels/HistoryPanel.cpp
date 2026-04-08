#include "luthpch.h"
#include "luth/editor/panels/HistoryPanel.h"
#include "luth/editor/CommandHistory.h"
#include "luth/editor/Command.h"
#include "luth/utils/LuthIcons.h"

#include <imgui.h>

namespace Luth
{
    HistoryPanel::HistoryPanel()
    {
        LH_CORE_INFO("Created History panel");
    }

    void HistoryPanel::OnInit() {}

    static void DrawCompoundChildren(const CompoundCommand& compound)
    {
        for (auto& child : compound.GetChildren())
        {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextDisabled("  %s", child->GetName());
        }
    }

    void HistoryPanel::OnRender()
    {
        ImGui::PushFont(Editor::GetFASolid());
        if (ImGui::Begin(ICON_FA_CLOCK_ROTATE_LEFT "  History"))
        {
            auto& undoStack = CommandHistory::GetUndoStack();
            auto& redoStack = CommandHistory::GetRedoStack();

            // ── Header bar ──────────────────────────────────────
            {
                bool canUndo = CommandHistory::CanUndo();
                bool canRedo = CommandHistory::CanRedo();

                if (!canUndo) ImGui::BeginDisabled();
                if (ImGui::SmallButton(ICON_FA_ARROW_ROTATE_LEFT " Undo"))
                    CommandHistory::Undo();
                if (!canUndo) ImGui::EndDisabled();

                ImGui::SameLine();

                if (!canRedo) ImGui::BeginDisabled();
                if (ImGui::SmallButton(ICON_FA_ARROW_ROTATE_RIGHT " Redo"))
                    CommandHistory::Redo();
                if (!canRedo) ImGui::EndDisabled();

                ImGui::SameLine();

                bool empty = undoStack.empty() && redoStack.empty();
                if (empty) ImGui::BeginDisabled();
                if (ImGui::SmallButton(ICON_FA_TRASH " Clear"))
                    CommandHistory::Clear();
                if (empty) ImGui::EndDisabled();

                ImGui::SameLine();
                ImGui::TextDisabled("(%zu undo, %zu redo)", undoStack.size(), redoStack.size());
            }

            // ── Compound state indicator ────────────────────────
            if (CommandHistory::IsInCompound())
            {
                ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f),
                    "Recording compound: %s", CommandHistory::GetCompoundName());
            }

            ImGui::Separator();

            // ── Stack list ──────────────────────────────────────
            if (ImGui::BeginChild("HistoryList", ImVec2(0, 0), false))
            {
                constexpr ImGuiTableFlags tableFlags =
                    ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH;

                if (ImGui::BeginTable("HistoryTable", 2, tableFlags))
                {
                    ImGui::TableSetupColumn("Command", ImGuiTableColumnFlags_WidthStretch);
                    ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 30.0f);

                    // ── Redo stack (top, grayed out, reverse order) ──
                    if (!redoStack.empty())
                    {
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        ImGui::TextDisabled("── Redo ──");

                        for (i32 i = (i32)redoStack.size() - 1; i >= 0; --i)
                        {
                            auto& cmd = redoStack[i];
                            ImGui::TableNextRow();
                            ImGui::TableSetColumnIndex(0);

                            auto* compound = dynamic_cast<CompoundCommand*>(cmd.get());
                            if (compound)
                            {
                                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.5f, 0.5f, 1.0f));
                                bool open = ImGui::TreeNodeEx(cmd->GetName(),
                                    ImGuiTreeNodeFlags_SpanAvailWidth);
                                ImGui::PopStyleColor();

                                ImGui::TableSetColumnIndex(1);
                                ImGui::TextDisabled("%zu", compound->GetChildCount());

                                if (open)
                                {
                                    DrawCompoundChildren(*compound);
                                    ImGui::TreePop();
                                }
                            }
                            else
                            {
                                ImGui::TextDisabled("%s", cmd->GetName());
                            }

                            // Next-to-redo marker
                            if (i == (i32)redoStack.size() - 1)
                            {
                                ImGui::TableSetColumnIndex(1);
                                ImGui::TextColored(ImVec4(0.3f, 0.7f, 1.0f, 1.0f), ICON_FA_ARROW_ROTATE_RIGHT);
                            }
                        }

                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        ImGui::Separator();
                    }

                    // ── Cursor (current state) ──
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.3f, 1.0f), ICON_FA_ARROW_RIGHT " Current State");

                    if (!undoStack.empty())
                    {
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        ImGui::Separator();

                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        ImGui::Text("── Undo ──");
                    }

                    // ── Undo stack (newest on top) ──
                    for (i32 i = (i32)undoStack.size() - 1; i >= 0; --i)
                    {
                        auto& cmd = undoStack[i];
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);

                        auto* compound = dynamic_cast<CompoundCommand*>(cmd.get());
                        if (compound)
                        {
                            bool highlight = (i == (i32)undoStack.size() - 1);
                            if (highlight)
                                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.9f, 0.4f, 1.0f));

                            bool open = ImGui::TreeNodeEx(cmd->GetName(),
                                ImGuiTreeNodeFlags_SpanAvailWidth);

                            if (highlight)
                                ImGui::PopStyleColor();

                            ImGui::TableSetColumnIndex(1);
                            ImGui::Text("%zu", compound->GetChildCount());

                            if (open)
                            {
                                DrawCompoundChildren(*compound);
                                ImGui::TreePop();
                            }
                        }
                        else
                        {
                            bool highlight = (i == (i32)undoStack.size() - 1);
                            if (highlight)
                                ImGui::TextColored(ImVec4(1.0f, 0.9f, 0.4f, 1.0f), "%s", cmd->GetName());
                            else
                                ImGui::Text("%s", cmd->GetName());
                        }

                        // Next-to-undo marker
                        if (i == (i32)undoStack.size() - 1)
                        {
                            ImGui::TableSetColumnIndex(1);
                            ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), ICON_FA_ARROW_ROTATE_LEFT);
                        }
                    }

                    ImGui::EndTable();
                }
            }
            ImGui::EndChild();
        }
        ImGui::End();
        ImGui::PopFont();
    }
}
