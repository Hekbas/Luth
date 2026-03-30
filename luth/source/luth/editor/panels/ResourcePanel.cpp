#include "luthpch.h"
#include "luth/editor/panels/ResourcePanel.h"
#include "luth/editor/EditorSelection.h"
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
                ImGuiTableFlags_BordersInnerV |
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
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(2, 2));
            ImGui::Checkbox("Models",    &m_ShowModels);
            ImGui::Checkbox("Textures",  &m_ShowTextures);
            ImGui::Checkbox("Materials", &m_ShowMaterials);
            ImGui::Checkbox("Shaders",   &m_ShowShaders);
            ImGui::Checkbox("Fonts",     &m_ShowFonts);
            ImGui::Checkbox("Scenes",    &m_ShowScenes);
            ImGui::PopStyleVar();
        });
    }

    void ResourcePanel::SetupColumns()
    {
        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 40);
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultSort, 200);
        ImGui::TableSetupColumn("Refs", ImGuiTableColumnFlags_WidthFixed, 50);
        ImGui::TableSetupColumn("UUID", ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_NoSort);
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
            if (metadata.Type == AssetType::Model    && !m_ShowModels)    continue;
            if (metadata.Type == AssetType::Material && !m_ShowMaterials) continue;
            if (metadata.Type == AssetType::Texture  && !m_ShowTextures)  continue;
            if (metadata.Type == AssetType::Shader   && !m_ShowShaders)   continue;
            if (metadata.Type == AssetType::Font     && !m_ShowFonts)     continue;
            if (metadata.Type == AssetType::Scene    && !m_ShowScenes)    continue;

            // Create entry
            ResourceEntry entry;
            entry.Name = metadata.Path.filename().string();
            entry.Uuid = uuid;

            switch (metadata.Type)
            {
            case AssetType::Model:    entry.Type = "Model";    break;
            case AssetType::Material: entry.Type = "Material"; break;
            case AssetType::Texture:  entry.Type = "Texture";  break;
            case AssetType::Shader:   entry.Type = "Shader";   break;
            case AssetType::Font:     entry.Type = "Font";     break;
            case AssetType::Scene:    entry.Type = "Scene";    break;
            default:                  entry.Type = "Unknown";  break;
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

        // Sorting
        if (ImGuiTableSortSpecs* specs = ImGui::TableGetSortSpecs())
        {
            if (specs->SpecsCount > 0)
            {
                const ImGuiTableColumnSortSpecs& s = specs->Specs[0];
                bool asc = (s.SortDirection == ImGuiSortDirection_Ascending);
                switch (s.ColumnIndex)
                {
                case 0: // Type
                    std::sort(m_FilteredResources.begin(), m_FilteredResources.end(),
                        [asc](const ResourceEntry& a, const ResourceEntry& b) {
                            return asc ? a.Type < b.Type : a.Type > b.Type;
                        });
                    break;
                case 1: // Name
                    std::sort(m_FilteredResources.begin(), m_FilteredResources.end(),
                        [asc](const ResourceEntry& a, const ResourceEntry& b) {
                            return asc ? a.Name < b.Name : a.Name > b.Name;
                        });
                    break;
                case 2: // Refs
                    std::sort(m_FilteredResources.begin(), m_FilteredResources.end(),
                        [asc](const ResourceEntry& a, const ResourceEntry& b) {
                            return asc ? a.RefCount < b.RefCount : a.RefCount > b.RefCount;
                        });
                    break;
                }
            }
            specs->SpecsDirty = false;
        }

        // Display entries
        for (const auto& entry : m_FilteredResources) {
            ImGui::TableNextRow();

            // Type column (also handle the span-all row selectable here)
            ImGui::TableSetColumnIndex(0);
            bool selected = (m_SelectedUUID == entry.Uuid);
            std::string selectableId = "##sel_" + entry.Uuid.ToString();
            
            if (ImGui::Selectable(selectableId.c_str(), selected,
                ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowItemOverlap,
                ImVec2(0, 0)))
            {
                m_SelectedUUID = entry.Uuid;
                EditorSelection::SelectResource(entry.Uuid);
            }
            ImGui::SameLine();
            ImGui::TextColored(GetTypeColor(entry.Type), "%s", GetTypeIcon(entry.Type));

            // Name column
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%s", entry.Name.c_str());
            if (ImGui::IsItemHovered() && entry.Name.size() > 24)
            {
                ImGui::BeginTooltip();
                ImGui::Text("%s", entry.Name.c_str());
                ImGui::EndTooltip();
            }

            // Refs column
            ImGui::TableSetColumnIndex(2);
            ImGui::Text("%d", entry.RefCount);

            // UUID column
            ImGui::TableSetColumnIndex(3);
            ImGui::TextDisabled("%s", entry.Uuid.ToString().c_str());
            if (ImGui::IsItemHovered()) {
                ImGui::BeginTooltip();
                ImGui::Text("UUID: %s", entry.Uuid.ToString().c_str());
                ImGui::EndTooltip();
            }
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
            {"Shader",   ImVec4(0.9f, 0.3f, 0.3f, 1.0f)},
            {"Font",     ImVec4(0.7f, 0.7f, 0.7f, 1.0f)},
            {"Scene",    ImVec4(0.7f, 0.4f, 0.9f, 1.0f)},
        };
        auto it = colors.find(type);
        return (it != colors.end()) ? it->second : ImVec4(1, 1, 1, 1);
    }

    const char* ResourcePanel::GetTypeIcon(const std::string& type) const
    {
        if (type == "Model")    return ICON_FA_CUBE;
        if (type == "Texture")  return ICON_FA_IMAGE;
        if (type == "Material") return ICON_FA_CIRCLE_HALF_STROKE;
        if (type == "Shader")   return ICON_FA_FILE_CODE;
        if (type == "Font")     return ICON_FA_FONT;
        if (type == "Scene")    return ICON_FA_FILM;
        return ICON_FA_FILE;
    }
}
