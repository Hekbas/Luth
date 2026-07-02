#include "lepch.h"
#include "luthien/widgets/ButtonGroup.h"
#include "luthien/widgets/Icons.h"
#include "luthien/Editor.h"

#include <imgui.h>
#include <cstdio>

namespace Luth::UI
{
    namespace
    {
        // Push the active highlight (Button + ButtonHovered) so the toggled idx stays visually pressed
        // even when not under the cursor. Matches the ScenePanel ToolButton lambda this helper subsumes.
        void PushActiveStyle()
        {
            const ImVec4 c = ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive);
            ImGui::PushStyleColor(ImGuiCol_Button, c);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, c);
        }
        void PopActiveStyle() { ImGui::PopStyleColor(2); }

        // Icon-button size. Height = GetFrameHeight (row alignment with normal widgets). Width adds
        // 2*(FramePadding.x - FramePadding.y) so icons get the same horizontal padding the X-pad implies;
        // strict-square sizing would only honor Y-pad and look pinched at default {6,4}.
        ImVec2 IconBtnSize()
        {
            const ImGuiStyle& s = ImGui::GetStyle();
            const float dx = std::max(0.0f, s.FramePadding.x - s.FramePadding.y);
            const float h  = ImGui::GetFrameHeight();
            return ImVec2(h + 2.0f * dx, h);
        }
    }

    bool SegmentedButton(const char* groupId, const char* const* labels, int count, int* selectedIdx, bool fillWidth)
    {
        if (!labels || !selectedIdx || count <= 0) return false;

        ImGui::PushID(groupId);
        const ImGuiStyle& style = ImGui::GetStyle();
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, style.ItemSpacing.y));

        const float btnW = fillWidth ? ImGui::GetContentRegionAvail().x / (float)count : 0.0f;

        bool changed = false;
        for (int i = 0; i < count; ++i)
        {
            const bool active = (*selectedIdx == i);
            if (active) PushActiveStyle();
            ImGui::PushID(i);
            if (ImGui::Button(labels[i], ImVec2(btnW, 0.0f)) && !active)
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

    bool IconToggleGroup(const char* groupId, const char* const* icons, const char* const* tooltips, int count, int* selectedIdx, const bool* filled)
    {
        if (!icons || !selectedIdx || count <= 0) return false;

        ImGui::PushID(groupId);
        const ImVec2 btn = IconBtnSize();

        bool changed = false;
        for (int i = 0; i < count; ++i)
        {
            const bool active = (*selectedIdx == i);
            if (active) PushActiveStyle();
            ImGui::PushID(i);
            if (filled && filled[i]) ImGui::PushFont(Editor::GetIconFill());
            const bool clicked = ImGui::Button(icons[i], btn);
            if (filled && filled[i]) ImGui::PopFont();
            if (clicked && !active)
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

        const ImVec2 btn = IconBtnSize();
        const bool active = *state;
        if (active) PushActiveStyle();

        // Embed the caller-supplied label as the ImGui ID so duplicate icons across the toolbar
        // don't collide.
        char buf[128];
        std::snprintf(buf, sizeof(buf), "%s##%s", icon ? icon : "", label ? label : "btn");
        const bool clicked = ImGui::Button(buf, btn);

        if (active) PopActiveStyle();
        if (tooltip && ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", tooltip);
        if (clicked) *state = !*state;
        return clicked;
    }

    bool SplitToggleButton(const char* groupId, const char* icon, const char* tooltip,
                           bool* state, const std::function<void()>& dropdownBody, bool filled)
    {
        ImGui::PushID(groupId);

        const ImVec2 btn    = IconBtnSize();
        const float  btnH   = btn.y;
        const float  chevW  = btnH * 0.75f;     // wider so the caret glyph isn't clipped
        const bool   active = state && *state;
        bool         toggled = false;

        // Icon half owns the toggle state if the caller passed one. When state is null, treat the whole
        // split as one big dropdown opener (icon-click does the same thing as chevron-click).
        if (active) PushActiveStyle();
        if (filled) ImGui::PushFont(Editor::GetIconFill());
        const bool iconClicked = ImGui::Button(icon ? icon : "##icon", btn);
        if (filled) ImGui::PopFont();
        if (iconClicked) {
            if (state) { *state = !*state; toggled = true; }
            else       { ImGui::OpenPopup("##split_popup"); }
        }
        if (active) PopActiveStyle();
        if (tooltip && ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", tooltip);
        const ImVec2 iconMin = ImGui::GetItemRectMin();

        // Chevron half sits flush against the icon (zero spacing).
        ImGui::SameLine(0.0f, 0.0f);
        ImGui::PushFont(Editor::GetIconFill());
        const bool chevClicked = ImGui::Button(ICON_CARET_DOWN_FILL "##chev", ImVec2(chevW, btnH));
        ImGui::PopFont();
        if (chevClicked)
            ImGui::OpenPopup("##split_popup");
        const float popupY = ImGui::GetItemRectMax().y;

        // Anchor the popup to the bottom-left of the split button (Unity style).
        ImGui::SetNextWindowPos(ImVec2(iconMin.x, popupY));
        if (ImGui::BeginPopup("##split_popup")) {
            if (dropdownBody) dropdownBody();
            ImGui::EndPopup();
        }

        ImGui::PopID();
        return toggled;
    }
}
