#include "luthpch.h"
#include "luth/editor/inspectors/SceneViewer.h"
#include "luth/editor/Editor.h"
#include "luth/editor/UI.h"
#include "luth/resources/FileSystem.h"
#include <nlohmann/json.hpp>

namespace Luth
{
    void SceneViewer::Draw(const UUID& sceneUUID, const fs::path& scenePath)
    {
        // Scene Info
        if (UI::BeginCollapsingHeader("Scene Info", true))
        {
            size_t fileSize = 0;
            int entityCount = 0;

            if (fs::exists(scenePath))
            {
                fileSize = fs::file_size(scenePath);

                // Parse scene JSON to count entities
                std::ifstream file(scenePath);
                if (file.is_open())
                {
                    try
                    {
                        nlohmann::json sceneJson = nlohmann::json::parse(file);
                        if (sceneJson.contains("entities") && sceneJson["entities"].is_array())
                            entityCount = (int)sceneJson["entities"].size();
                    }
                    catch (...) {}
                }
            }

            if (UI::BeginInfoTable("SceneProps")) {
                UI::InfoRow("Entities", "%d", entityCount);

                // Format file size
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

        // Load Scene button
        float buttonWidth = ImGui::CalcTextSize("Load Scene").x + ImGui::GetStyle().FramePadding.x * 2.0f;
        ImGui::SetCursorPosX((ImGui::GetContentRegionAvail().x - buttonWidth) * 0.5f);
        if (ImGui::Button("Load Scene"))
        {
            Editor::OpenScene(scenePath);
        }
    }
}
