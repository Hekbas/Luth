#include "lepch.h"
#include "luthien/widgets/FilterBox.h"
#include "luthien/widgets/Icons.h"

#include <imgui.h>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>

namespace Luth::UI
{
    bool FilterBox(const char* id, char* buf, std::size_t bufSize, const char* hint)
    {
        if (!buf || bufSize == 0) return false;

        ImGui::PushID(id);
        const bool  hasText = buf[0] != '\0';
        const float frameH  = ImGui::GetFrameHeight();
        const float clearW  = hasText ? frameH + ImGui::GetStyle().ItemSpacing.x : 0.0f;

        char hintBuf[160];
        std::snprintf(hintBuf, sizeof(hintBuf), ICON_SEARCH "  %s", hint ? hint : "Search...");

        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - clearW);
        bool changed = ImGui::InputTextWithHint("##in", hintBuf, buf, bufSize);

        if (hasText)
        {
            ImGui::SameLine();
            if (ImGui::Button(ICON_CLOSE "##clear", ImVec2(frameH, frameH)))
            {
                buf[0] = '\0';
                changed = true;
            }
        }

        ImGui::PopID();
        return changed;
    }

    bool PassesFilter(const char* filter, const char* text)
    {
        if (!filter || !filter[0]) return true;
        if (!text) return false;

        std::string h = text, n = filter;
        std::transform(h.begin(), h.end(), h.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        return h.find(n) != std::string::npos;
    }
}
