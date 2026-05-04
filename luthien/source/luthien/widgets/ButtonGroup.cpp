#include "lepch.h"
#include "luthien/widgets/ButtonGroup.h"

#include <imgui.h>
#include <cstdio>

namespace Luth::UI
{
    namespace
    {
        // Push the active highlight (Button + ButtonHovered) so the toggled idx
        // stays visually pressed even when not under the cursor. Matches the
        // ScenePanel ToolButton lambda this helper subsumes.
        void PushActiveStyle()
        {
            const ImVec4 c = ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive);
            ImGui::PushStyleColor(ImGuiCol_Button, c);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, c);
        }
        void PopActiveStyle() { ImGui::PopStyleColor(2); }
    }

    bool SegmentedButton(const char* groupId, const char* const* labels, int count, int* selectedIdx)
    {
        if (!labels || !selectedIdx || count <= 0) return false;

        ImGui::PushID(groupId);
        const ImGuiStyle& style = ImGui::GetStyle();
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, style.ItemSpacing.y));

        bool changed = false;
        for (int i = 0; i < count; ++i)
        {
            const bool active = (*selectedIdx == i);
            if (active) PushActiveStyle();
            ImGui::PushID(i);
            if (ImGui::Button(labels[i]) && !active)
            {
                *selectedIdx = i;
                changed = true;
            }
            ImGui::PopID();
            if (active) PopActiveStyle();
            if (i + 1 < count) ImGui::SameLine();
        }

        ImGui::PopStyleVar();
        ImGui::PopID();
        return changed;
    }

    bool IconToggleGroup(const char* groupId, const char* const* icons, const char* const* tooltips, int count, int* selectedIdx)
    {
        if (!icons || !selectedIdx || count <= 0) return false;

        ImGui::PushID(groupId);
        const float btnSize = ImGui::GetFrameHeight();

        bool changed = false;
        for (int i = 0; i < count; ++i)
        {
            const bool active = (*selectedIdx == i);
            if (active) PushActiveStyle();
            ImGui::PushID(i);
            if (ImGui::Button(icons[i], ImVec2(btnSize, btnSize)) && !active)
            {
                *selectedIdx = i;
                changed = true;
            }
            ImGui::PopID();
            if (active) PopActiveStyle();
            if (tooltips && tooltips[i] && ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", tooltips[i]);
            if (i + 1 < count) ImGui::SameLine(0.0f, 2.0f);
        }

        ImGui::PopID();
        return changed;
    }

    bool IconToggleButton(const char* label, const char* icon, const char* tooltip, bool* state)
    {
        if (!state) return false;

        const float btnSize = ImGui::GetFrameHeight();
        const bool active = *state;
        if (active) PushActiveStyle();

        // Embed the caller-supplied label as the ImGui ID so duplicate icons
        // across the toolbar don't collide.
        char buf[128];
        std::snprintf(buf, sizeof(buf), "%s##%s", icon ? icon : "", label ? label : "btn");
        const bool clicked = ImGui::Button(buf, ImVec2(btnSize, btnSize));

        if (active) PopActiveStyle();
        if (tooltip && ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", tooltip);
        if (clicked) *state = !*state;
        return clicked;
    }
}
