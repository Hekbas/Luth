#include "luthpch.h"
#include "luth/editor/inspectors/FontViewer.h"
#include "luth/editor/UI.h"

namespace Luth
{
    void FontViewer::Draw(const UUID& fontUUID, const fs::path& fontPath)
    {
        // Font Info
        if (ImGui::CollapsingHeader("Font Info", ImGuiTreeNodeFlags_DefaultOpen))
        {
            std::string fontName = fontPath.stem().string();

            size_t fileSize = 0;
            if (fs::exists(fontPath))
                fileSize = fs::file_size(fontPath);

            UI::BeginInfoTable("FontProps");
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

        ImGui::Dummy({ 0, 8 });

        // Preview
        if (ImGui::CollapsingHeader("Preview", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::TextWrapped("ABCDEFGHIJKLMNOPQRSTUVWXYZ");
            ImGui::TextWrapped("abcdefghijklmnopqrstuvwxyz");
            ImGui::TextWrapped("0123456789 !@#$%%^&*()");
            ImGui::Dummy({ 0, 4 });
            ImGui::TextWrapped("The quick brown fox jumps over the lazy dog.");
        }
    }
}
