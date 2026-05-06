#include "lepch.h"
#include "luthien/panels/ResourcePanel.h"
#include "luthien/EditorSelection.h"
#include "luthien/EditorSnapshot.h"
#include "luth/resources/AssetDatabase.h"
#include "luth/resources/AssetManager.h"
#include "luthien/widgets/ImGuiUtils.h"
#include "luthien/widgets/Icons.h"

namespace Luth
{
    ResourcePanel::ResourcePanel()
    {
        m_WindowID = "Resources";
        LH_CORE_INFO("Created Resource panel");
    }

    void ResourcePanel::OnInit()
    {
        // Bump dirty flag whenever the asset registry changes; main-thread fan-out
        // happens via the same AssetDatabase callback ProjectPanel uses.
        AssetDatabase::AddChangeCallback([this]() { m_NeedsRebuild = true; });
    }

    void ResourcePanel::OnGather(EditorSnapshotBuilder& builder)
    {
        // PopulateData (asset DB enumeration + sort/filter) stays inline for v2.9.0;
        // future polish can shift it here.
        builder.Add<ResourceListSnapshot>();
    }

    void ResourcePanel::OnDraw(const EditorSnapshot& /*snapshot*/)
    {
        LH_PROFILE_FUNCTION();
        ImGui::PushFont(Editor::GetFASolid());
        std::string resources = ICON_FA_DATABASE + std::string("  Resources");

        if (BeginWindow(resources.c_str()))
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
        if (ImGui::InputTextWithHint("##Search", ICON_FA_MAGNIFYING_GLASS, m_SearchBuffer, IM_ARRAYSIZE(m_SearchBuffer)))
            m_NeedsRebuild = true;

        ImGui::SameLine();

        ButtonDropdown(ICON_FA_FILTER, "type_filter", [this]() {
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(2, 2));
            bool changed = false;
            changed |= ImGui::Checkbox("Models",    &m_ShowModels);
            changed |= ImGui::Checkbox("Textures",  &m_ShowTextures);
            changed |= ImGui::Checkbox("Materials", &m_ShowMaterials);
            changed |= ImGui::Checkbox("Shaders",   &m_ShowShaders);
            changed |= ImGui::Checkbox("Fonts",     &m_ShowFonts);
            changed |= ImGui::Checkbox("Scenes",    &m_ShowScenes);
            if (changed) m_NeedsRebuild = true;
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

    void ResourcePanel::RebuildIfDirty()
    {
        if (!m_NeedsRebuild) return;
        m_NeedsRebuild = false;

        m_FilteredResources.clear();

        const auto& registry = AssetDatabase::GetRegistry();
        if (registry.empty()) return;

        m_FilteredResources.reserve(registry.size());

        for (const auto& [uuid, metadata] : registry)
        {
            // Filter by type
            if (metadata.Type == AssetType::Model    && !m_ShowModels)    continue;
            if (metadata.Type == AssetType::Material && !m_ShowMaterials) continue;
            if (metadata.Type == AssetType::Texture  && !m_ShowTextures)  continue;
            if (metadata.Type == AssetType::Shader   && !m_ShowShaders)   continue;
            if (metadata.Type == AssetType::Font     && !m_ShowFonts)     continue;
            if (metadata.Type == AssetType::Scene    && !m_ShowScenes)    continue;

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

            if (auto asset = AssetManager::GetAsset<Asset>(uuid))
                entry.RefCount = asset.use_count() - 1;  // AssetManager holds one ref
            else
                entry.RefCount = 0;

            if (ResourceMatchesSearch(entry))
                m_FilteredResources.push_back(std::move(entry));
        }

        // Sort using current TableSortSpecs (caller is inside BeginTable scope).
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
        }
    }

    void ResourcePanel::RefreshDynamicData()
    {
        // RefCount drifts as AssetManager hands out / drops shared_ptrs without
        // bumping the registry callback. Re-read it cheaply each frame; only
        // re-sort when the user is sorting by that column AND a value moved.
        bool refsChanged = false;
        for (auto& entry : m_FilteredResources)
        {
            int newRef = 0;
            if (auto asset = AssetManager::GetAsset<Asset>(entry.Uuid))
                newRef = asset.use_count() - 1;
            if (entry.RefCount != newRef) {
                entry.RefCount = newRef;
                refsChanged = true;
            }
        }

        if (!refsChanged) return;

        if (ImGuiTableSortSpecs* specs = ImGui::TableGetSortSpecs())
        {
            if (specs->SpecsCount > 0 && specs->Specs[0].ColumnIndex == 2)
            {
                bool asc = (specs->Specs[0].SortDirection == ImGuiSortDirection_Ascending);
                std::sort(m_FilteredResources.begin(), m_FilteredResources.end(),
                    [asc](const ResourceEntry& a, const ResourceEntry& b) {
                        return asc ? a.RefCount < b.RefCount : a.RefCount > b.RefCount;
                    });
            }
        }
    }

    void ResourcePanel::PopulateData()
    {
        // Sort-spec edits ride the same dirty path. SpecsDirty is consumed here
        // so the rebuild runs exactly once after the user clicks a header.
        if (ImGuiTableSortSpecs* specs = ImGui::TableGetSortSpecs())
        {
            if (specs->SpecsDirty) {
                m_NeedsRebuild = true;
                specs->SpecsDirty = false;
            }
        }

        RebuildIfDirty();
        RefreshDynamicData();

        // Display entries — clipped so off-screen rows skip the per-row work.
        ImGuiListClipper clipper;
        clipper.Begin((int)m_FilteredResources.size(), ImGui::GetTextLineHeightWithSpacing());
        while (clipper.Step())
        {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row)
            {
                const auto& entry = m_FilteredResources[row];
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
