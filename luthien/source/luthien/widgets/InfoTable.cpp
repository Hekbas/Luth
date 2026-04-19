#include "lepch.h"
#include "luthien/widgets/InfoTable.h"

#include <imgui.h>
#include <cstdarg>


namespace Luth::UI
{
    bool BeginInfoTable(const char* id)
    {
        return ImGui::BeginTable(id, 2, ImGuiTableFlags_SizingStretchSame);
    }

    void InfoRow(const char* label, const char* fmt, ...)
    {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::Text("%s", label);
        ImGui::TableSetColumnIndex(1);

        va_list args;
        va_start(args, fmt);
        ImGui::TextV(fmt, args);
        va_end(args);
    }

    void EndInfoTable()
    {
        ImGui::EndTable();
    }
}
