#pragma once

namespace Luth::UI
{
    // ImGui Begin/EndTable pair for the two-column label / value rows used throughout the
    // inspector. The printf-style InfoRow keeps value formatting at the call site so callers
    // don't have to duplicate the table boilerplate every time.
    bool BeginInfoTable(const char* id);
    void InfoRow(const char* label, const char* fmt, ...);
    void EndInfoTable();
}
