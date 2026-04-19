#pragma once

#include "luth/resources/importers/ImportReport.h"

#include <filesystem>
#include <string>

namespace Luth
{
    // Modal dialog for resolving textures missing after a model import.
    // Call Open(report) once, then Draw() every frame until it returns true.
    class TextureRemapDialog
    {
    public:
        static void Open(const ImportReport& report);

        // Returns true on the frame the dialog closes (resolved or skipped).
        static bool Draw();

        static bool IsOpen() { return s_Open; }

    private:
        static void ApplyResolutions();

        static bool s_Open;
        static ImportReport s_Report;

        // Indexed by ImportReport::Unresolved entry.
        static std::vector<std::string> s_UserPaths;
        static char s_SearchDir[512];
    };
}
