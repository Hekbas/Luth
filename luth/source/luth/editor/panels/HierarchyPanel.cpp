#include "luthpch.h"
#include "luth/editor/panels/HierarchyPanel.h"
#include "luth/scene/Components.h"

#include <imgui.h>
#include <imgui_internal.h>

namespace Luth
{
    HierarchyPanel::HierarchyPanel(Scene* context)
        : m_Context(context)
    {
        LH_CORE_INFO("Created Hierarchy panel");
    }

    void HierarchyPanel::OnRender()
    {
        if (ImGui::Begin("Hierarchy"))
        {
            // Header with search and create button
            ImGui::AlignTextToFramePadding();

            if (ImGui::Button("+"))
                m_ShowCreateMenu = true;

            ImGui::SameLine();
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
            ImGui::InputTextWithHint("##Search", "Search...", m_SearchFilter, IM_ARRAYSIZE(m_SearchFilter));

            // Entity list
            ImGui::Separator();
            ImGui::BeginChild("EntityList");

            m_Context->EachEntity([&](Entity entity) {
                if (!entity.HasParent() && EntityMatchesFilter(entity)) {
                    DrawEntityNode(entity);
                }
            });

            // Handle drag drop target
            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ENTITY")) {
                    Entity droppedEntity = *(Entity*)payload->Data;
                    LH_CORE_INFO("Dropped entity {} onto hierarchy root", droppedEntity.GetName());
                    droppedEntity.RemoveParent();
                }
                ImGui::EndDragDropTarget();
            }

            ImGui::EndChild();

            // Context menus
            if (ImGui::BeginPopupContextWindow()) {
                DrawEntityCreateMenu();
                ImGui::EndPopup();
            }

            if (m_ShowCreateMenu) {
                ImGui::OpenPopup("CreateEntityMenu");
                m_ShowCreateMenu = false;
            }

            if (ImGui::BeginPopup("CreateEntityMenu")) {
                DrawEntityCreateMenu();
                ImGui::EndPopup();
            }

            ProcessKeyboardShortcuts();
        }
        
        ImGui::End();
    }

    void HierarchyPanel::SetSelectedEntity(Entity entity)
    {
        if (m_Selection != entity) {
            // Clear selection if entity is invalid
            if (!entity) {
                LH_CORE_TRACE("Cleared selection");
                m_Selection = {};
                return;
            }

            // Ensure entity still exists in registry
            if (!m_Context->Registry().valid(entity)) {
                LH_CORE_WARN("Tried to select invalid entity");
                m_Selection = {};
                return;
            }

            LH_CORE_TRACE("Changed selection to {0}", entity.GetName());
            m_Selection = entity;
        }
    }

    void HierarchyPanel::DrawEntityNode(Entity entity)
    {
        auto name = entity.GetName();
        ImGuiTreeNodeFlags flags =
            ImGuiTreeNodeFlags_OpenOnArrow |
            ImGuiTreeNodeFlags_SpanAvailWidth |
            (m_Selection == entity ? ImGuiTreeNodeFlags_Selected : 0) |
            (entity.GetChildren().empty() ? ImGuiTreeNodeFlags_Leaf : 0);

        ImGui::PushID(entity.GetComponent<ID>().m_ID);
        bool isOpen = ImGui::TreeNodeEx((void*)(uint64_t)entity, flags, "%s", name.c_str());
        ImGui::PopID();

        // Handle selection
        if (ImGui::IsItemClicked()) {
            SetSelectedEntity(entity);
        }

        // Drag and drop
        if (ImGui::BeginDragDropSource()) {
            ImGui::SetDragDropPayload("ENTITY", &entity, sizeof(Entity));
            ImGui::Text("Move %s", name.c_str());
            m_DraggedEntity = entity;
            ImGui::EndDragDropSource();
        }

        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ENTITY")) {
                Entity child = *(Entity*)payload->Data;
                child.SetParent(entity);
            }
            ImGui::EndDragDropTarget();
        }

        // Context menu
        if (ImGui::BeginPopupContextItem()) {
            DrawEntityContextMenu(entity);
            ImGui::EndPopup();
        }

        // Children recursion
        if (isOpen) {
            for (const auto &child : entity.GetChildren()) {
                DrawEntityNode(child);
            }
            ImGui::TreePop();
        }
    }

    void HierarchyPanel::DrawEntityContextMenu(Entity entity)
    {
        if (ImGui::MenuItem("Rename"))
            entity.BeginRename();

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
            LH_CORE_INFO("Created child entity {0}", child.GetName());
        }

        if (ImGui::MenuItem("Duplicate")) {
            auto duplicate = m_Context->DuplicateEntity(entity);
            LH_CORE_INFO("Duplicated entity {0}", duplicate.GetName());
        }
    }

    void HierarchyPanel::DrawEntityCreateMenu()
    {
        if (ImGui::MenuItem("Empty Entity")) {
            auto entity = m_Context->CreateEntity("New Entity");
        }

        if (ImGui::BeginMenu("3D Objects")) {
            if (ImGui::MenuItem("Cube")) { /* Create mesh entity */ }
            if (ImGui::MenuItem("Sphere")) { /* Create mesh entity */ }
            ImGui::EndMenu();
        }

        if (ImGui::MenuItem("Camera")) {
            auto camera = m_Context->CreateEntity("Camera");
            //camera.AddComponent<CameraComponent>();
            LH_CORE_INFO("Created camera entity");
        }
    }

    void HierarchyPanel::ProcessKeyboardShortcuts()
    {
        if (ImGui::IsWindowFocused()) {
            // Delete key
            if (ImGui::IsKeyPressed(ImGuiKey_Delete) && m_Selection) {
                m_Context->DestroyEntity(m_Selection);
                SetSelectedEntity({});
                LH_CORE_INFO("Deleted selected entity");
            }

            // Ctrl+D for duplicate
            if (ImGui::IsKeyDown(ImGuiKey_LeftCtrl) && ImGui::IsKeyPressed(ImGuiKey_D)) {
                if (m_Selection) {
                    auto duplicate = m_Context->DuplicateEntity(m_Selection);
                    LH_CORE_INFO("Duplicated entity {0}", duplicate.GetName());
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
}
