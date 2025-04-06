#include "luthpch.h"
#include "luth/editor/panels/ResourcePanel.h"
#include "luth/resources/libraries/MaterialLibrary.h"
#include "luth/resources/libraries/ModelLibrary.h"
#include "luth/resources/libraries/ShaderLibrary.h"
#include "luth/resources/libraries/TextureCache.h"
#include "luth/utils/ImGuiUtils.h"

namespace Luth
{
    ResourcePanel::ResourcePanel()
    {
        LH_CORE_INFO("Created Resource panel");
    }

    void ResourcePanel::OnInit() {}

    void ResourcePanel::OnRender()
    {
        if (ImGui::Begin("Resources"))
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
    }

    void ResourcePanel::DrawFilterControls()
    {
        ImGui::SetNextItemWidth(200);
        ImGui::InputTextWithHint("##Search", "Search...", m_SearchBuffer, IM_ARRAYSIZE(m_SearchBuffer));

        ImGui::SameLine();

        ButtonDropdown("Type Filter", "type_filter", [this]() {
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

        // Collect resources from libraries
        if (m_ShowModels)    AddModelEntries();
        if (m_ShowMaterials) AddMaterialEntries();
        if (m_ShowShaders)   AddShaderEntries();
        if (m_ShowTextures)  AddTextureEntries();

        // Display entries
        for (const auto& entry : m_FilteredResources) {
            if (!ResourceMatchesSearch(entry)) continue;

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

    void ResourcePanel::AddModelEntries()
    {
        for (const auto& [uuid, model] : ModelLibrary::GetAllModels()) {
            m_FilteredResources.push_back({
                model.Model->GetName(),
                uuid,
                "Model",
                model.Model.use_count() - 1 // Subtract library's own reference
            });
        }
    }

    void ResourcePanel::AddMaterialEntries()
    {
        for (const auto& [uuid, material] : MaterialLibrary::GetAllMaterials()) {
            m_FilteredResources.push_back({
                material->GetName(),
                uuid,
                "Material",
                material.use_count() - 1
            });
        }
    }

    void ResourcePanel::AddShaderEntries()
    {
        for (const auto& [uuid, shader] : ShaderLibrary::GetAllShaders()) {
            m_FilteredResources.push_back({
                shader.Shader->GetName(),
                uuid,
                "Shader",
                shader.Shader.use_count() - 1
            });
        }
    }

    void ResourcePanel::AddTextureEntries()
    {
        for (const auto& [uuid, texture] : TextureCache::GetAllTextures()) {
            m_FilteredResources.push_back({
                texture.Texture->GetName(),
                uuid,
                "Texture",
                texture.Texture.use_count() - 1
            });
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
