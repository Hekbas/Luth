#include "luthpch.h"
#include "luth/editor/panels/InspectorPanel.h"
#include "luth/editor/panels/HierarchyPanel.h"
#include "luth/scene/Components.h"
#include "luth/utils/CustomImGui.h"

namespace Luth
{
    InspectorPanel::InspectorPanel()
    {
        LH_CORE_INFO("Created Inspector panel");
    }

    void InspectorPanel::OnInit() {}

    void InspectorPanel::OnRender()
    {
        if (ImGui::Begin("Inspector"))
        {
            if (!m_SelectedEntity) {
                AlignTextToCenter("No entity selected");
                ImGui::End();
                return;
            }

            // Display and edit the entity's Tag component (name)
            if (m_SelectedEntity.HasComponent<Tag>()) {
                auto& tag = m_SelectedEntity.GetComponent<Tag>();

                // Create a horizontal group for checkbox + name
                ImGui::BeginGroup();

                // Checkbox for active state
                bool isActive = m_SelectedEntity.IsActive();
                if (ImGui::Checkbox("##Active", &isActive)) {
                    m_SelectedEntity.SetActive(isActive);
                }

                // Tooltip and spacing
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Toggle Entity Active State");
                }
                ImGui::SameLine();

                // Name field
                char buffer[256];
                strcpy(buffer, tag.m_Tag.c_str());
                ImGui::PushItemWidth(-1);
                if (ImGui::InputText("##Name", buffer, sizeof(buffer))) {
                    tag.m_Tag = std::string(buffer);
                }

                ImGui::EndGroup();
                ImGui::Dummy({ 0, 4 });
            }

            // Draw each component with a collapsible UI section
            DrawComponent<ID>("ID", m_SelectedEntity, [](Entity entity, ID& component) {
                ImGui::Text("ID: %llu", component.m_ID);
            });

            DrawComponent<Parent>("Parent", m_SelectedEntity, [](Entity entity, Parent& component) {
                if (component.m_Parent && component.m_Parent.IsValid()) {
                    ImGui::Text("Parent: %s", component.m_Parent.GetName().c_str());
                    if (ImGui::Button("Clear Parent")) {
                        entity.SetParent({});
                    }
                }
                else {
                    ImGui::Text("No Parent");
                }
            });

            DrawComponent<Children>("Children", m_SelectedEntity, [](Entity entity, Children& component) {
                ImGui::Text("Children: %d", component.m_Children.size());
                for (auto& child : component.m_Children) {
                    if (child.IsValid()) {
                        ImGui::BulletText("%s", child.GetName().c_str());
                    }
                    else {
                        ImGui::BulletText("Invalid Entity");
                    }
                }
            });

            DrawComponent<Transform>("Transform", m_SelectedEntity, [](Entity entity, Transform& transform) {
                    // Position control
                ImGui::Text("Position"); ImGui::SameLine();
                ImGui::PushItemWidth(-1);
                ImGui::DragFloat3("##Position", glm::value_ptr(transform.m_Position), 0.1f);

                // Rotation control (Euler angles)
                ImGui::Text("Rotation"); ImGui::SameLine();
                glm::vec3 rotationDegrees = transform.m_Rotation;
                if (ImGui::DragFloat3("##Rotation", glm::value_ptr(rotationDegrees), 0.5f)) {
                    transform.m_Rotation = rotationDegrees;
                }

                // Scale control
                ImGui::Text("Scale"); ImGui::SameLine(); ImGui::Dummy({15, 0}); ImGui::SameLine();
                ImGui::DragFloat3("##Scale", glm::value_ptr(transform.m_Scale), 0.1f);

                // Reset buttons
                if (ImGui::Button("Reset Transform")) {
                    transform.m_Position = { 0,0,0 };
                    transform.m_Rotation = { 0,0,0 };
                    transform.m_Scale    = { 1,1,1 };
                }
            });

            // Add Component button
            ImGui::Separator();
            ImGui::Dummy({ 0, 4 });
            AlignItemToCenter(100);
            ButtonDropdown("Add Component", "inspector_addcomponent", [this]() {
                if (!m_SelectedEntity.HasComponent<Tag>() && ImGui::MenuItem("Tag")) {
                    m_SelectedEntity.AddOrReplaceComponent<Tag>();
                    ImGui::CloseCurrentPopup();
                }
                if (!m_SelectedEntity.HasComponent<Parent>() && ImGui::MenuItem("Parent")) {
                    m_SelectedEntity.AddOrReplaceComponent<Parent>();
                    ImGui::CloseCurrentPopup();
                }
                if (!m_SelectedEntity.HasComponent<Children>() && ImGui::MenuItem("Children")) {
                    m_SelectedEntity.AddOrReplaceComponent<Children>();
                    ImGui::CloseCurrentPopup();
                }
                // Add more components here as needed
            });
        }
        ImGui::End();
    }

    template<typename T, typename UIFunction>
    void InspectorPanel::DrawComponent(const std::string& name, Entity entity, UIFunction uiFunction)
    {
        const ImGuiTreeNodeFlags treeNodeFlags = ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_Framed
            | ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_AllowItemOverlap | ImGuiTreeNodeFlags_FramePadding;

        if (entity.HasComponent<T>()) {
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2{ 4, 4 });
            bool open = ImGui::TreeNodeEx(name.c_str(), treeNodeFlags);
            ImGui::PopStyleVar();

            // Right-click to open context menu for component removal
            bool removeComponent = false;
            if (ImGui::BeginPopupContextItem()) {
                constexpr bool enabled = (std::is_same_v<T, Transform> || std::is_same_v<T, ID>) ? false : true;
                removeComponent = ImGui::MenuItem("Remove component", nullptr, nullptr, enabled);
                ImGui::EndPopup();
            }

            if (open) {
                uiFunction(entity, entity.GetComponent<T>());
                ImGui::TreePop();
            }

            if (removeComponent) {
                entity.RemoveComponent<T>();
            }

            ImGui::Dummy({0, 4});
        }
    }
}
