#include "lepch.h"
#include "luthien/panels/InspectorPanel.h"
#include "luthien/EditorSnapshot.h"
#include "luthien/inspectors/ComponentDrawerRegistry.h"
#include "luthien/widgets/Widgets.h"
#include "luthien/commands/Commands.h"
#include "luthien/CommandHistory.h"
#include "luth/scene/Components.h"
#include "luth/resources/AssetDatabase.h"
#include "luth/resources/AssetManager.h"
#include "luth/renderer/resources/Model.h"
#include "luth/renderer/material/Material.h"
#include "luth/renderer/resources/Texture.h"
#include "luth/renderer/shader/Shader.h"
#include "luth/resources/FileSystem.h"
#include "luthien/widgets/ImGuiUtils.h"
#include "luthien/widgets/Icons.h"

namespace Luth
{
    using namespace Component;

    InspectorPanel::InspectorPanel()
    {
        m_WindowID = "Inspector";
        LH_CORE_INFO("Created Inspector panel");
    }

    void InspectorPanel::OnInit() {}

    void InspectorPanel::OnGather(EditorSnapshotBuilder& builder)
    {
        // v2.9.0: structural migration — fragment carries change-detection state.
        // Component-drawer property reads (and, importantly, MaterialEditor's shader-combo
        // enumeration that dominated the pre-rework Tracy capture) remain inline in OnDraw.
        // Moving the asset-DB lookups + property reads into gather is a future polish epic.
        auto* snap = builder.Add<InspectorSnapshot>();
        snap->selectionVersion = EditorSelection::GetVersion();
        snap->locked = m_IsLocked;
    }

    void InspectorPanel::OnDraw(const EditorSnapshot& /*snapshot*/)
    {
        LH_PROFILE_FUNCTION();
        ImGui::PushFont(Editor::GetFASolid());
        std::string inspector = ICON_FA_CIRCLE_INFO + std::string("  Inspector");

        if (BeginWindow(inspector.c_str()))
        {
            // Clear lock if entity becomes invalid
            if (m_IsLocked && !m_LockedEntity.IsValid()) {
                m_IsLocked = false;
                m_LockedEntity = {};
            }

            Entity selectedEntity = m_IsLocked ? m_LockedEntity : EditorSelection::GetSelectedEntity();
            UUID selectedResource = EditorSelection::GetSelectedResource();

            if (selectedEntity) {
                DrawEntityComponents(selectedEntity);
            }
            else if (!m_IsLocked && selectedResource.IsValid()) {
                DrawResourceProperties(selectedResource);
            }
        }
        ImGui::End();
        ImGui::PopFont();
    }

    void InspectorPanel::DrawEntityComponents(Entity m_SelectedEntity)
    {
        // Display and edit the entity's Tag component (name)
        if (m_SelectedEntity.HasComponent<Tag>()) {
            auto& tag = m_SelectedEntity.GetComponent<Tag>();

            // Horizontal group: [checkbox] [name...............] [lock]
            ImGui::BeginGroup();

            bool isActive = m_SelectedEntity.IsActive();
            if (ImGui::Checkbox("##Active", &isActive)) {
                CommandHistory::Execute(std::make_unique<EntityActiveCommand>(
                    m_SelectedEntity.GetScene(), (entt::entity)m_SelectedEntity, !isActive, isActive));
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Toggle Entity Active State");
            ImGui::SameLine();

            // Name field — reserve right margin for the lock button
            float lockBtnWidth = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.x;
            ImGui::PushItemWidth(-lockBtnWidth);
            char buffer[256];
            strncpy(buffer, tag.Value.c_str(), sizeof(buffer) - 1);
            buffer[sizeof(buffer) - 1] = 0;
            if (ImGui::InputText("##Name", buffer, sizeof(buffer))) {
                std::string oldName = tag.Value;
                std::string newName = buffer;
                if (oldName != newName) {
                    CommandHistory::Execute(std::make_unique<EntityRenameCommand>(
                        m_SelectedEntity.GetScene(), (entt::entity)m_SelectedEntity, oldName, newName));
                }
            }
            ImGui::PopItemWidth();
            ImGui::SameLine();

            // Lock button — right-anchored in the same row
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
            const char* lockIcon = m_IsLocked ? ICON_FA_LOCK : ICON_FA_LOCK_OPEN;
            if (ImGui::Button(lockIcon, ImVec2(ImGui::GetFrameHeight(), ImGui::GetFrameHeight()))) {
                m_IsLocked = !m_IsLocked;
                if (m_IsLocked)
                    m_LockedEntity = EditorSelection::GetSelectedEntity();
                else
                    m_LockedEntity = {};
            }
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(m_IsLocked ? "Unlock Inspector" : "Lock to Current Entity");

            ImGui::EndGroup();
            ImGui::Dummy({ 0, 4 });
        }
        m_ActiveMaterialUUID = UUID::Invalid();
        for (const auto& d : ComponentDrawerRegistry::GetDrawers())
            d.Draw(m_SelectedEntity);

        // Add Component button
        ImGui::Separator();
        ImGui::Dummy({ 0, 4 });
        AlignItemToCenter(100);
        ButtonDropdown("Add Component", "inspector_addcomponent", [&m_SelectedEntity]() {
            for (const auto& d : ComponentDrawerRegistry::GetDrawers()) {
                if (!d.ShowInAddMenu) continue;
                if (!d.CanAdd(m_SelectedEntity)) continue;
                if (ImGui::MenuItem(d.Name.c_str())) {
                    d.OnAdd(m_SelectedEntity);
                    ImGui::CloseCurrentPopup();
                }
            }
        });

        if (m_ActiveMaterialUUID.IsValid()) {
            if (!AssetManager::IsLoaded(m_ActiveMaterialUUID) && !AssetManager::IsLoading(m_ActiveMaterialUUID))
                AssetManager::LoadAsync(m_ActiveMaterialUUID);

            if (auto mat = AssetManager::GetAsset<Material>(m_ActiveMaterialUUID)) {
                ImGui::Dummy({ 0, 8 });
                ImGui::Separator();
                ImGui::Dummy({ 0, 4 });
                m_MaterialEditor.Draw(*mat);
            }
        }
    }

    void InspectorPanel::DrawResourceProperties(UUID m_SelectedResource)
    {
        const auto& meta = AssetDatabase::GetMetadata(m_SelectedResource);

        // Handle invalid/deleted assets — silently clear stale selection
        if (meta.Type == AssetType::None)
        {
            EditorSelection::ClearSelection();
            return;
        }

        AssetType type = meta.Type;

        // Always show Metadata
        if (UI::BeginCollapsingHeader("Asset Metadata", true))
        {
            if (ImGui::BeginTable("Metadata", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
            {
                ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0); ImGui::Text("Name"); ImGui::TableSetColumnIndex(1); ImGui::Text("%s", meta.Path.filename().string().c_str());
                ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0); ImGui::Text("Type"); ImGui::TableSetColumnIndex(1); ImGui::Text("%s", FileSystem::GetTypeInfo().at(type).name.c_str());
                ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0); ImGui::Text("UUID"); ImGui::TableSetColumnIndex(1); ImGui::TextDisabled("%s", m_SelectedResource.ToString().c_str());
                ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0); ImGui::Text("Path"); ImGui::TableSetColumnIndex(1); ImGui::TextWrapped("%s", meta.Path.string().c_str());
                ImGui::EndTable();
            }
            UI::EndCollapsingHeader();
        }
        ImGui::Dummy({ 0, 4 });

        // Scene and Font don't go through the asset pipeline — display directly
        if (type == AssetType::Scene) {
            m_SceneViewer.Draw(m_SelectedResource, meta.Path);
            return;
        }
        if (type == AssetType::Font) {
            m_FontViewer.Draw(m_SelectedResource, meta.Path);
            return;
        }

        // Load on inspect if not loaded
        if (!AssetManager::IsLoaded(m_SelectedResource)) {
            if (!AssetManager::IsLoading(m_SelectedResource)) {
                AssetManager::LoadAsync(m_SelectedResource);
            }

            ImGui::Text("Loading Asset Data...");
            return;
        }

        // Delegate to specialized editors
        if (type == AssetType::Model) {
            if (auto model = AssetManager::GetAsset<Model>(m_SelectedResource)) m_ModelViewer.Draw(*model);
        } else if (type == AssetType::Material) {
            if (auto mat = AssetManager::GetAsset<Material>(m_SelectedResource)) m_MaterialEditor.Draw(*mat);
        } else if (type == AssetType::Texture) {
            if (auto tex = AssetManager::GetAsset<Texture>(m_SelectedResource)) m_TextureEditor.Draw(*tex);
        } else if (type == AssetType::Shader) {
            if (auto shader = AssetManager::GetAsset<Shader>(m_SelectedResource)) m_ShaderEditor.Draw(*shader);
        }
    }
}
