#pragma once

#include "luth/resources/importers/ImportReport.h"

#include <filesystem>
#include <string>

namespace Luth
{
    // Modal dialog that appears after a model import when textures could not be resolved
    // automatically. Lets the user browse or search a directory to find them.
    //
    // Usage (from Editor::Render or similar):
    //   TextureRemapDialog::Open(report);
    //   TextureRemapDialog::Draw();   // call every frame while editor is rendering
    class TextureRemapDialog
    {
    public:
        // Open the dialog with the given import report (copies the data).
        static void Open(const ImportReport& report);

        // Render the dialog. Must be called every ImGui frame.
        // Returns true when the dialog was just closed (resolved or skipped).
        static bool Draw();

        static bool IsOpen() { return s_Open; }

    private:
        static void ApplyResolutions();

        static bool s_Open;
        static ImportReport s_Report;

        // Per-entry user-provided path (indexed by Unresolved vector index)
        static std::vector<std::string> s_UserPaths;

        // "Search directory" field
        static char s_SearchDir[512];
    };
}
