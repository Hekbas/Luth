#include "luthpch.h"
#include "luth/editor/panels/HierarchyPanel.h"
#include "luth/editor/panels/ProjectPanel.h"
#include "luth/ECS/Components.h"
#include "luth/resources/Resources.h"
#include "luth/resources/FileSystem.h"
#include "luth/resources/ResourceDB.h"
#include "luth/renderer/Renderer.h"
#include "luth/ECS/Systems.h"
#include "luth/utils/ImGuiUtils.h"

#include <imgui.h>
#include <imgui_internal.h>

namespace Luth
{
    HierarchyPanel::HierarchyPanel()
    {
        LH_CORE_INFO("Created Hierarchy panel");
        m_Context = std::make_shared<Scene>();
    }

    void HierarchyPanel::OnInit()
    {
        Systems::SetRegistry(m_Context->RegistryPtr());
    }

    void HierarchyPanel::OnRender()
    {
        if (ImGui::Begin("Hierarchy"))
        {
            // Header with search and create button
            ImGui::AlignTextToFramePadding();

            ButtonDropdown("+", "hierarchy_+", [this]() { DrawEntityCreateMenu(); });

            ImGui::SameLine();
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
            ImGui::InputTextWithHint("##Search", "Search...", m_SearchFilter, IM_ARRAYSIZE(m_SearchFilter));

            // Entity list
            ImGui::Separator();
            if (ImGui::BeginChild("EntityList"))
            {
                m_Context->EachEntity([&](Entity entity) {
                    if (!entity.HasParent() && EntityMatchesFilter(entity)) {
                        DrawEntityNode(entity);
                    }
                });

                // Handle drag drop target
                if (ImGui::BeginDragDropTarget()) {
                    if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ENTITY")) {
                        Entity droppedEntity = *(Entity*)payload->Data;
                        LH_CORE_INFO("Dropped entity {0} onto hierarchy root", droppedEntity.GetName());
                        droppedEntity.RemoveParent();
                    }
                    ImGui::EndDragDropTarget();
                }
            }
            ImGui::EndChild();

            // Context menus
            if (ImGui::BeginPopupContextWindow()) {
                DrawEntityCreateMenu();
                ImGui::EndPopup();
            }

            ProcessKeyboardShortcuts();

            // Create Entities from dropped stuff
            ProcessDropResource();
        }
        
        ImGui::End();
    }

    void HierarchyPanel::SetSelectedEntity(Entity entity)
    {
        if (m_Selection != entity) {
            // Clear selection if entity is invalid
            if (!entity) {
                //LH_CORE_TRACE("Cleared selection");
                m_Selection = {};
                return;
            }

            // Ensure entity still exists in registry
            if (!m_Context->Registry().valid(entity)) {
                LH_CORE_WARN("Tried to select invalid entity");
                m_Selection = {};
                return;
            }

            //LH_CORE_TRACE("Changed selection to {0}", entity.GetName());
            m_Selection = entity;
            if (auto* inspector = Editor::GetPanel<InspectorPanel>()) {
                inspector->SetSelectedEntity(entity);
            }
        }
    }

    void HierarchyPanel::DrawEntityNode(Entity entity)
    {
        if (!entity.IsValid()) return;
        const std::string name = entity.GetName();
        bool isRenaming = (m_RenamingEntity == entity);

        ImGuiTreeNodeFlags flags =
            ImGuiTreeNodeFlags_DefaultOpen |
            ImGuiTreeNodeFlags_OpenOnArrow |
            (m_Selection == entity ? ImGuiTreeNodeFlags_Selected : 0) |
            (entity.GetChildren().empty() ? ImGuiTreeNodeFlags_Leaf : 0);

        ImGui::PushID(static_cast<int>(entity.GetComponent<ID>().m_ID));

        // Split arrow and text into separate interactable areas
        bool isOpen = ImGui::TreeNodeEx("##TreeNodeArrow", flags);

        // Handle arrow interactions
        if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
            SetSelectedEntity(entity);
        }

        // Text label (separate interactable area)
        ImGui::SameLine();

        if (isRenaming) {
            // Rename input field
            bool finish = false;
            bool cancel = false;
            ImGuiInputTextFlags flags = ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll;
            ImGui::SetKeyboardFocusHere();
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
            if (ImGui::InputText("##Rename", m_RenameBuffer, sizeof(m_RenameBuffer), flags)) {
                finish = true;
            }

            // Handle Escape key
            if (ImGui::IsItemActive() && ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                cancel = true;
            }

            // Finalize renaming
            if (finish || cancel) {
                if (finish && !cancel) {
                    // Validate and apply new name
                    std::string newName(m_RenameBuffer);
                    if (newName.empty()) newName = m_OriginalName;
                    entity.GetComponent<Tag>().m_Tag = newName;
                }
                else if (cancel) {
                    // Restore original name
                    entity.GetComponent<Tag>().m_Tag = m_OriginalName;
                }
                m_RenamingEntity = {};
                m_OriginalName.clear();
            }
        }
        else {
            // Clickable text label
            ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, ImVec2(0.0f, 25.0f));
            if (ImGui::Selectable(name.c_str(), m_Selection == entity,
                ImGuiSelectableFlags_AllowDoubleClick | ImGuiSelectableFlags_SpanAllColumns))
            {
                SetSelectedEntity(entity);

                // Handle double-click on text only
                if (ImGui::IsMouseDoubleClicked(0)) {
                    m_RenamingEntity = entity;
                    strncpy_s(m_RenameBuffer, name.c_str(), sizeof(m_RenameBuffer));
                }
            }
            ImGui::PopStyleVar();

            // Drag and drop on text area
            HandleDragDrop(entity, name);
        }

        // Context menu (works on both areas)
        if (ImGui::BeginPopupContextItem()) {
            if (ImGui::MenuItem("Rename")) {
                m_RenamingEntity = entity;
                strncpy_s(m_RenameBuffer, name.c_str(), sizeof(m_RenameBuffer));
            }
            DrawEntityContextMenu(entity);
            ImGui::EndPopup();
        }

        // Child nodes
        if (isOpen) {
            for (auto child : entity.GetChildren()) {
                DrawEntityNode(child);
            }
            ImGui::TreePop();
        }

        ImGui::PopID();
    }

    void HierarchyPanel::DrawEntityContextMenu(Entity entity)
    {
        if (ImGui::MenuItem("Delete")) {
            if (m_Selection == entity)
                SetSelectedEntity({});
            m_Context->DestroyEntity(entity);
            LH_CORE_INFO("Deleted entity {0}", entity.GetName());
        }

        ImGui::Separator();

        if (ImGui::MenuItem("Create Child")) {
            auto child = m_Context->CreateEntity("Child Entity");
            child.SetParent(entity);
        }

        if (ImGui::MenuItem("Duplicate")) {
            auto duplicate = m_Context->DuplicateEntity(entity);
        }
    }

    void HierarchyPanel::DrawEntityCreateMenu()
    {
        if (ImGui::MenuItem("Empty Entity")) {
            auto entity = m_Context->CreateEntity("New Entity");
        }

        if (ImGui::BeginMenu("3D Objects")) {
            if (ImGui::MenuItem("Cube")) { /* TODO: Create mesh entity */ }
            if (ImGui::MenuItem("Sphere")) { /* TODO: Create mesh entity */ }
            ImGui::EndMenu();
        }

        if (ImGui::MenuItem("Camera")) {
            auto camera = m_Context->CreateEntity("Camera");
            camera.AddComponent<Camera>();
        }

        if (ImGui::BeginMenu("Light")) {
            if (ImGui::MenuItem("Directional Light")) {
                auto camera = m_Context->CreateEntity("Directional Light");
                camera.AddComponent<DirectionalLight>();
                camera.GetComponent<Transform>().m_Rotation = Vec3(-1);
            }
            if (ImGui::MenuItem("Point Light")) {
                auto camera = m_Context->CreateEntity("Point Light");
                camera.AddComponent<PointLight>();
            }
            ImGui::EndMenu();
        }
    }

    void HierarchyPanel::HandleDragDrop(Entity entity, const std::string& name)
    {
        if (ImGui::BeginDragDropSource()) {
            ImGui::SetDragDropPayload("ENTITY", &entity, sizeof(Entity));
            ImGui::Text("Move %s", name.c_str());
            ImGui::EndDragDropSource();
        }

        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ENTITY")) {
                Entity child = *(Entity*)payload->Data;
                if (!child.IsAncestorOf(entity)) {
                    child.SetParent(entity);
                }
            }
            ImGui::EndDragDropTarget();
        }
    }

    void HierarchyPanel::ProcessKeyboardShortcuts()
    {
        if (ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows)) {
            // [SUPR] Delete
            if (ImGui::IsKeyPressed(ImGuiKey_Delete) && m_Selection) {
                m_Context->DestroyEntity(m_Selection);
                SetSelectedEntity({});
                LH_CORE_INFO("Deleted selected entity");
            }

            // [Ctrl+D] Duplicate
            if (ImGui::IsKeyDown(ImGuiKey_LeftCtrl) && ImGui::IsKeyPressed(ImGuiKey_D)) {
                if (m_Selection) {
                    auto duplicate = m_Context->DuplicateEntity(m_Selection);
                }
            }
        }
    }

    bool HierarchyPanel::EntityMatchesFilter(Entity entity)
    {
        if (strlen(m_SearchFilter) == 0) return true;

        // Case-insensitive search
        std::string entityName = entity.GetName();
        std::string filter = m_SearchFilter;

        std::transform(entityName.begin(), entityName.end(), entityName.begin(), ::tolower);
        std::transform(filter.begin(), filter.end(), filter.begin(), ::tolower);

        return entityName.find(filter) != std::string::npos;
    }

    void HierarchyPanel::ProcessDropResource()
    {
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(ASSET_UUID)) {
                const UUID assetUuid = *static_cast<const UUID*>(payload->Data);
                auto assetType = ResourceDB::UuidToInfo(assetUuid).Type;

                switch (assetType) {
                    case ResourceType::Model: {
                        std::shared_ptr<Model> model = ModelLibrary::Get(assetUuid);
                        auto parent = m_Context->CreateEntity(model->GetName());
                        parent.AddComponent<Children>();

                        if (model->IsSkinned()) parent.AddComponent<Animation>(assetUuid);

                        auto materials = model->GetMaterials();

                        int meshIndex = 0;
                        for (const auto& mesh : model->GetMeshesData()) {
                            auto child = m_Context->CreateEntity(mesh.Name);

                            child.AddComponent<Parent>();
                            child.SetParent(parent);
                            parent.GetChildren().push_back(child);

                            auto& meshRend = child.AddComponent<MeshRenderer>();
                            meshRend.modelNamePreview = model->GetName();
                            meshRend.ModelUUID = assetUuid;
                            meshRend.MeshIndex = meshIndex;
							meshRend.isSkinned = model->IsSkinned();
                            if (!model->GetMaterials().empty() && meshIndex < materials.size()) {
                                meshRend.MaterialUUID = materials[meshIndex];
                                meshRend.materialNamePreview = MaterialLibrary::Get(meshRend.MaterialUUID)->GetName();
                            }
                            meshIndex++;
                        }
                        break;
                    }
                    default: break;
                }
            }
            ImGui::EndDragDropTarget();
        }
    }
}
