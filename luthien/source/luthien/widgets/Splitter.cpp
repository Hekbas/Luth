#include "lepch.h"
#include "luthien/widgets/Splitter.h"

#include <imgui.h>

namespace Luth::UI
{
    bool Splitter(const char* id, float* bottomHeight, float thickness)
    {
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
        ImGui::InvisibleButton(id, ImVec2(-1, thickness));
        ImGui::PopStyleVar();

        const bool hovered = ImGui::IsItemHovered();
        const bool active  = ImGui::IsItemActive();
        if (hovered || active)
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);

        if (active && bottomHeight)
            *bottomHeight -= ImGui::GetIO().MouseDelta.y;

        // Subtle baseline (1-px center line) when idle; full-strip fill on
        // hover/active so the drag affordance is unmistakable when reached.
        const ImVec2 a = ImGui::GetItemRectMin();
        const ImVec2 b = ImGui::GetItemRectMax();
        if (active || hovered) {
            const ImU32 col = ImGui::GetColorU32(active ? ImGuiCol_SeparatorActive
                                                        : ImGuiCol_SeparatorHovered);
            ImGui::GetWindowDrawList()->AddRectFilled(a, b, col);
        } else {
            const ImU32 col = ImGui::GetColorU32(ImGuiCol_Separator);
            const float midY = (a.y + b.y) * 0.5f;
            ImGui::GetWindowDrawList()->AddLine({ a.x, midY }, { b.x, midY }, col);
        }

        return ImGui::IsItemDeactivated();
    }
}
