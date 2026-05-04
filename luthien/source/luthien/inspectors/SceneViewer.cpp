#include "lepch.h"
#include "luthien/inspectors/SceneViewer.h"
#include "luthien/Editor.h"
#include "luthien/widgets/Widgets.h"
#include "luth/resources/FileSystem.h"
#include <nlohmann/json.hpp>

namespace Luth
{
    void SceneViewer::Draw(const UUID& sceneUUID, const fs::path& scenePath)
    {
        // Scene Info
        if (UI::BeginCollapsingHeader("Scene Info", true))
        {
            // mtime check is a cheap stat; full read+parse only on actual change.
            std::error_code ec;
            const fs::file_time_type mtime = fs::exists(scenePath, ec)
                ? fs::last_write_time(scenePath, ec) : fs::file_time_type{};
            const bool fileExists = fs::exists(scenePath, ec);

            if (fileExists && (!m_Cache || m_Cache->uuid != sceneUUID || m_Cache->mtime != mtime))
            {
                CacheEntry e;
                e.uuid     = sceneUUID;
                e.mtime    = mtime;
                e.fileSize = fs::file_size(scenePath, ec);

                std::ifstream file(scenePath);
                if (file.is_open()) {
                    try {
                        nlohmann::json sceneJson = nlohmann::json::parse(file);
                        if (sceneJson.contains("entities") && sceneJson["entities"].is_array())
                            e.entityCount = (int)sceneJson["entities"].size();
                    } catch (...) {}
                }
                m_Cache = std::move(e);
            }
            else if (!fileExists) {
                m_Cache.reset();
            }

            const int entityCount = m_Cache ? m_Cache->entityCount : 0;
            const std::uintmax_t fileSize = m_Cache ? m_Cache->fileSize : 0;

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
