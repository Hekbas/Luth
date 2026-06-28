#include "lepch.h"
#include "luthien/widgets/CategoryPopup.h"
#include "luthien/widgets/FilterBox.h"

#include <imgui.h>
#include <cstring>

namespace Luth::UI
{
    bool CategoryList(const char* id, const CategoryItem* items, int count,
                      int* current, char* filterBuf, std::size_t filterBufSize)
    {
        if (!items || !current || count <= 0) return false;

        ImGui::PushID(id);
        FilterBox("##filter", filterBuf, filterBufSize, "Filter modes...");
        ImGui::Separator();

        bool        changed      = false;
        const char* lastCategory = nullptr;

        for (int i = 0; i < count; ++i)
        {
            const CategoryItem& it = items[i];
            if (!PassesFilter(filterBuf, it.label)) continue;

            // Header prints lazily on the first visible row of each group, so a group fully
            // hidden by the filter leaves no orphan header.
            if (!lastCategory || std::strcmp(lastCategory, it.category) != 0)
            {
                if (lastCategory) ImGui::Spacing();
                lastCategory = it.category;
                ImGui::TextDisabled("%s", it.category);
            }

            if (!it.enabled) ImGui::BeginDisabled();
            if (ImGui::RadioButton(it.label, *current == it.value) && it.enabled)
            {
                *current = it.value;
                changed  = true;
            }
            if (!it.enabled) ImGui::EndDisabled();

            if (!it.enabled && it.disabledReason &&
                ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("%s", it.disabledReason);
        }

        ImGui::PopID();
        return changed;
    }
}
