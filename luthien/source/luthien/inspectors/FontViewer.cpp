#include "lepch.h"
#include "luthien/inspectors/FontViewer.h"
#include "luthien/UI.h"

namespace Luth
{
    void FontViewer::Draw(const UUID& fontUUID, const fs::path& fontPath)
    {
        // Font Info
        if (UI::BeginCollapsingHeader("Font Info", true))
        {
            std::string fontName = fontPath.stem().string();

            size_t fileSize = 0;
            if (fs::exists(fontPath))
                fileSize = fs::file_size(fontPath);

            if (UI::BeginInfoTable("FontProps")) {
                UI::InfoRow("Name", "%s", fontName.c_str());
                UI::InfoRow("Extension", "%s", fontPath.extension().string().c_str());

                if (fileSize < 1024)
                    UI::InfoRow("File Size", "%d B", (int)fileSize);
                else if (fileSize < 1024 * 1024)
                    UI::InfoRow("File Size", "%.1f KB", fileSize / 1024.0f);
                else
                    UI::InfoRow("File Size", "%.1f MB", fileSize / (1024.0f * 1024.0f));

                UI::EndInfoTable();
            }
            UI::EndCollapsingHeader();
        }

        ImGui::Dummy({ 0, 8 });

        // Preview
        if (UI::BeginCollapsingHeader("Preview", true))
        {
            ImGui::TextWrapped("ABCDEFGHIJKLMNOPQRSTUVWXYZ");
            ImGui::TextWrapped("abcdefghijklmnopqrstuvwxyz");
            ImGui::TextWrapped("0123456789 !@#$%%^&*()");
            ImGui::Dummy({ 0, 4 });
            ImGui::TextWrapped("The quick brown fox jumps over the lazy dog.");
            UI::EndCollapsingHeader();
        }
    }
}
