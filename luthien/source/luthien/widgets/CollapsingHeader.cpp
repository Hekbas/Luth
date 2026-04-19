#include "lepch.h"
#include "luthien/widgets/CollapsingHeader.h"
#include "luthien/Editor.h"
#include "luthien/widgets/Icons.h"

#include <imgui.h>
#include <imgui/imgui_internal.h>


namespace Luth::UI
{
    bool BeginCollapsingHeader(const char* label, bool defaultOpen, const std::function<void()>& contextMenu)
    {
        ImGuiWindow* window = ImGui::GetCurrentWindow();
        if (window->SkipItems) return false;

        ImGuiID id = window->GetID(label);
        bool is_open = window->StateStorage.GetInt(id, defaultOpen ? 1 : 0) != 0;

        ImVec2 pos = window->DC.CursorPos;
        ImVec2 size = ImVec2(ImGui::GetContentRegionAvail().x, ImGui::GetFrameHeight() + 8.0f);
        ImRect bb(pos, ImVec2(pos.x + size.x, pos.y + size.y));

        ImGui::ItemSize(bb);
        if (!ImGui::ItemAdd(bb, id))
        {
            if (is_open)
            {
                ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.15f, 0.15f, 0.15f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.24f, 0.24f, 0.24f, 1.0f));
                ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 4.0f);
                ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0f);
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 8.0f));

                char child_id[128];
                snprintf(child_id, sizeof(child_id), "##%s_content", label);
                ImGui::BeginChild(child_id, ImVec2(0, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
            }
            return is_open;
        }

        bool hovered, held;
        bool pressed = ImGui::ButtonBehavior(bb, id, &hovered, &held);
        if (pressed)
        {
            is_open = !is_open;
            window->StateStorage.SetInt(id, is_open ? 1 : 0);
        }

        if (contextMenu)
        {
            std::string popupId = "HeaderContextMenu_" + std::string(label);
            if (hovered && ImGui::IsMouseReleased(ImGuiMouseButton_Right))
                ImGui::OpenPopup(popupId.c_str());

            if (ImGui::BeginPopup(popupId.c_str()))
            {
                contextMenu();
                ImGui::EndPopup();
            }
        }

        ImU32 bg_col;
        if (is_open)
            bg_col = ImGui::GetColorU32(ImVec4(0.24f, 0.24f, 0.24f, 1.0f));
        else if (hovered)
            bg_col = ImGui::GetColorU32(ImGuiCol_HeaderHovered);
        else
            bg_col = ImGui::GetColorU32(ImGuiCol_Header);

        window->DrawList->AddRectFilled(bb.Min, bb.Max, bg_col, 4.0f, is_open ? ImDrawFlags_RoundCornersTop : ImDrawFlags_RoundCornersAll);

        ImGui::PushFont(Editor::GetFASolid());
        const char* icon = is_open ? ICON_FA_CARET_DOWN : ICON_FA_CARET_RIGHT;
        ImVec2 icon_size = ImGui::CalcTextSize(icon);
        ImVec2 icon_pos = ImVec2(bb.Min.x + 8.0f, bb.Min.y + (size.y - icon_size.y) * 0.5f);
        window->DrawList->AddText(icon_pos, ImGui::GetColorU32(ImGuiCol_Text), icon);
        ImGui::PopFont();

        ImVec2 text_size = ImGui::CalcTextSize(label);
        ImVec2 text_pos = ImVec2(bb.Min.x + 28.0f, bb.Min.y + (size.y - text_size.y) * 0.5f);
        window->DrawList->AddText(text_pos, ImGui::GetColorU32(ImGuiCol_Text), label);

        float handle_height = 10.0f;
        ImVec2 handle_p = ImVec2(bb.Max.x - 16.0f, bb.Min.y + (size.y - handle_height) * 0.5f);
        ImU32 dot_color = ImGui::GetColorU32(ImGuiCol_TextDisabled);
        for (int x = 0; x < 2; x++)
        {
            for (int y = 0; y < 4; y++)
            {
                window->DrawList->AddRectFilled(
                    ImVec2(handle_p.x + x * 4.0f, handle_p.y + y * 3.0f),
                    ImVec2(handle_p.x + x * 4.0f + 2.0f, handle_p.y + y * 3.0f + 2.0f),
                    dot_color
                );
            }
        }

        if (is_open)
        {
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.15f, 0.15f, 0.15f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.24f, 0.24f, 0.24f, 1.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 4.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 8.0f));

            ImGui::SetCursorPosY(ImGui::GetCursorPosY() - ImGui::GetStyle().ItemSpacing.y);

            char child_id[128];
            snprintf(child_id, sizeof(child_id), "##%s_content", label);
            ImGui::BeginChild(child_id, ImVec2(0, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
        }

        return is_open;
    }

    void EndCollapsingHeader()
    {
        ImGui::EndChild();
        ImGui::PopStyleVar(3);
        ImGui::PopStyleColor(2);
        ImGui::Spacing();
    }
}
