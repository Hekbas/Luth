#include "luthpch.h"
#include "luth/editor/panels/ResourcePanel.h"
#include "luth/resources/AssetDatabase.h"
#include "luth/resources/AssetManager.h"
#include "luth/utils/ImGuiUtils.h"
#include "luth/utils/LuthIcons.h"

namespace Luth
{
    ResourcePanel::ResourcePanel()
    {
        LH_CORE_INFO("Created Resource panel");
    }

    void ResourcePanel::OnInit() {}

    void ResourcePanel::OnRender()
    {
        ImGui::PushFont(Editor::GetFASolid());
        std::string resources = ICON_FA_DATABASE + std::string("  Resources");

        if (ImGui::Begin(resources.c_str()))
        {
            // Filter controls
            DrawFilterControls();

            // Main table
            constexpr ImGuiTableFlags flags =
                ImGuiTableFlags_Resizable |
                ImGuiTableFlags_Borders |
                ImGuiTableFlags_Sortable |
                ImGuiTableFlags_RowBg |
                ImGuiTableFlags_ScrollY;

            if (ImGui::BeginTable("ResourceTable", 4, flags)) {
                SetupColumns();
                PopulateData();
                ImGui::EndTable();
            }
        }
        ImGui::End();
		ImGui::PopFont();
    }

    void ResourcePanel::DrawFilterControls()
    {
        ImGui::SetNextItemWidth(200);
        ImGui::InputTextWithHint("##Search", ICON_FA_MAGNIFYING_GLASS, m_SearchBuffer, IM_ARRAYSIZE(m_SearchBuffer));

        ImGui::SameLine();

        ButtonDropdown(ICON_FA_FILTER, "type_filter", [this]() {
            ImGui::Checkbox("Models", &m_ShowModels);
            ImGui::Checkbox("Textures", &m_ShowTextures);
            ImGui::Checkbox("Materials", &m_ShowMaterials);
            ImGui::Checkbox("Shaders", &m_ShowShaders);
        });
    }

    void ResourcePanel::SetupColumns()
    {
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed, 200);
        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 100);
        ImGui::TableSetupColumn("UUID", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Refs", ImGuiTableColumnFlags_WidthFixed, 50);
        ImGui::TableSetupScrollFreeze(0, 1); // Make header row visible
        ImGui::TableHeadersRow();
    }

    void ResourcePanel::PopulateData()
    {
        m_FilteredResources.clear();

        const auto& registry = AssetDatabase::GetRegistry();
        if (registry.empty()) return;
 
        for (const auto& [uuid, metadata] : registry)
        {
            // Filter by type
            if (metadata.Type == AssetType::Model && !m_ShowModels) continue;
            if (metadata.Type == AssetType::Material && !m_ShowMaterials) continue;
            if (metadata.Type == AssetType::Texture && !m_ShowTextures) continue;
            if (metadata.Type == AssetType::Shader && !m_ShowShaders) continue;
 
            // Create entry
            ResourceEntry entry;
            entry.Name = metadata.Path.filename().string();
            entry.Uuid = uuid;
             
            switch (metadata.Type)
            {
            case AssetType::Model:    entry.Type = "Model"; break;
            case AssetType::Material: entry.Type = "Material"; break;
            case AssetType::Texture:  entry.Type = "Texture"; break;
            case AssetType::Shader:   entry.Type = "Shader"; break;
            case AssetType::Font:     entry.Type = "Font"; break;
            case AssetType::Scene:    entry.Type = "Scene"; break;
            default:                  entry.Type = "Unknown"; break;
            }
 
            // Check if loaded in AssetManager
            if (auto asset = AssetManager::GetAsset<Asset>(uuid))
            {
                // -1 because AssetManager holds one reference
                entry.RefCount = asset.use_count() - 1; 
            }
            else
            {
                entry.RefCount = 0;
            }
 
            if (ResourceMatchesSearch(entry))
            {
                m_FilteredResources.push_back(entry);
            }
        }

        // Display entries
        for (const auto& entry : m_FilteredResources) {
            //if (!ResourceMatchesSearch(entry)) continue;

            ImGui::TableNextRow();

            // Name column
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("%s", entry.Name.c_str());

            // Type column
            ImGui::TableSetColumnIndex(1);
            ImGui::TextColored(GetTypeColor(entry.Type), "%s", entry.Type.c_str());

            // UUID column
            ImGui::TableSetColumnIndex(2);
            ImGui::TextDisabled("%s", entry.Uuid.ToString().c_str());
            if (ImGui::IsItemHovered()) {
                ImGui::BeginTooltip();
                ImGui::Text("UUID: %s", entry.Uuid.ToString().c_str());
                ImGui::EndTooltip();
            }

            // Ref count column
            ImGui::TableSetColumnIndex(3);
            ImGui::Text("%d", entry.RefCount);
        }
    }

    bool ResourcePanel::ResourceMatchesSearch(ResourceEntry entry)
    {
        if (strlen(m_SearchBuffer) == 0) return true;

        // Case-insensitive search
        std::string name = entry.Name;
        std::string uuid = entry.Uuid.ToString();
        std::string filter = m_SearchBuffer;

        std::transform(name.begin(), name.end(), name.begin(), ::tolower);
        std::transform(uuid.begin(), uuid.end(), uuid.begin(), ::tolower);
        std::transform(filter.begin(), filter.end(), filter.begin(), ::tolower);

        return (name.find(filter) != std::string::npos) || (uuid.find(filter) != std::string::npos);
    }

    ImVec4 ResourcePanel::GetTypeColor(const std::string& type) const
    {
        static const std::unordered_map<std::string, ImVec4> colors = {
            {"Model",    ImVec4(0.4f, 0.8f, 1.0f, 1.0f)},
            {"Texture",  ImVec4(0.8f, 0.6f, 0.2f, 1.0f)},
            {"Material", ImVec4(0.2f, 0.9f, 0.4f, 1.0f)},
            {"Shader",   ImVec4(0.9f, 0.3f, 0.3f, 1.0f)}
        };
        return colors.at(type);
    }
}
