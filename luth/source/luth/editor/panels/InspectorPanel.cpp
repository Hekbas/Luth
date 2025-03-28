#include "luthpch.h"
#include "luth/editor/panels/InspectorPanel.h"
#include "luth/editor/panels/HierarchyPanel.h"
#include "luth/scene/Components.h"


namespace Luth
{
    void InspectorPanel::OnInit() {}

    void InspectorPanel::OnRender()
    {
        if (ImGui::Begin("Inspector"))
        {
            if (!m_SelectedEntity) {
                ImGui::Text("No entity selected");
                ImGui::End();
                return;
            }

            // Display and edit the entity's Tag component (name)
            if (m_SelectedEntity.HasComponent<Component::Tag>()) {
                auto& tag = m_SelectedEntity.GetComponent<Component::Tag>();
                char buffer[256];
                strcpy(buffer, tag.m_Tag.c_str());
                if (ImGui::InputText("##Name", buffer, sizeof(buffer))) {
                    tag.m_Tag = std::string(buffer);
                }
            }

            // Draw each component with a collapsible UI section
            DrawComponent<Component::ID>("ID", m_SelectedEntity, [](Entity entity, Component::ID& component) {
                ImGui::Text("ID: %llu", component.m_ID);
            });

            DrawComponent<Component::Parent>("Parent", m_SelectedEntity, [](Entity entity, Component::Parent& component) {
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

            DrawComponent<Component::Children>("Children", m_SelectedEntity, [](Entity entity, Component::Children& component) {
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

            // Add Component button
            ImGui::Separator();
            if (ImGui::Button("Add Component")) {
                ImGui::OpenPopup("AddComponent");
            }

            if (ImGui::BeginPopup("AddComponent")) {
                if (!m_SelectedEntity.HasComponent<Component::Tag>() && ImGui::MenuItem("Tag")) {
                    m_SelectedEntity.AddOrReplaceComponent<Component::Tag>();
                    ImGui::CloseCurrentPopup();
                }
                if (!m_SelectedEntity.HasComponent<Component::Parent>() && ImGui::MenuItem("Parent")) {
                    m_SelectedEntity.AddOrReplaceComponent<Component::Parent>();
                    ImGui::CloseCurrentPopup();
                }
                if (!m_SelectedEntity.HasComponent<Component::Children>() && ImGui::MenuItem("Children")) {
                    m_SelectedEntity.AddOrReplaceComponent<Component::Children>();
                    ImGui::CloseCurrentPopup();
                }
                // Add more components here as needed
                ImGui::EndPopup();
            }
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
                if (ImGui::MenuItem("Remove component")) {
                    removeComponent = true;
                }
                ImGui::EndPopup();
            }

            if (open) {
                uiFunction(entity, entity.GetComponent<T>());
                ImGui::TreePop();
            }

            if (removeComponent) {
                entity.RemoveComponent<T>();
            }
        }
    }
}
