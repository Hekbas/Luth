#pragma once

namespace Luth::UI
{
    bool BeginInfoTable(const char* id);
    void InfoRow(const char* label, const char* fmt, ...);
    void EndInfoTable();
}
