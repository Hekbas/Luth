#include "luthpch.h"
#include "luth/editor/panels/InspectorPanel.h"
#include "luth/editor/panels/HierarchyPanel.h"
#include "luth/scene/Components.h"
#include "luth/resources/ResourceDB.h"
#include "luth/resources/libraries/MaterialLibrary.h"
#include "luth/resources/libraries/ModelLibrary.h"
#include "luth/utils/ImGuiUtils.h"

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
            if (m_SelectedEntity) {
                DrawEntityComponents();
            }
            else if (m_SelectedResource) {
                DrawResourceProperties();
            }
        }
        ImGui::End();
    }

    void InspectorPanel::DrawEntityComponents()
    {
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
        #if defined(DEBUG)
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
        #endif

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
            ImGui::Text("Scale"); ImGui::SameLine(); ImGui::Dummy({ 15, 0 }); ImGui::SameLine();
            ImGui::DragFloat3("##Scale", glm::value_ptr(transform.m_Scale), 0.1f);

            // Reset buttons
            if (ImGui::Button("Reset Transform")) {
                transform.m_Position = { 0,0,0 };
                transform.m_Rotation = { 0,0,0 };
                transform.m_Scale = { 1,1,1 };
            }
        });

        DrawComponent<Camera>("Camera", m_SelectedEntity, [](Entity e, Camera& camera) {
            // Projection type combo box
            const char* projectionTypeStrings[] = { "Perspective", "Orthographic" };
            const char* currentProjectionType = projectionTypeStrings[(int)camera.Projection];

            if (ImGui::BeginCombo("Projection", currentProjectionType)) {
                for (int i = 0; i < 2; i++) {
                    bool isSelected = currentProjectionType == projectionTypeStrings[i];
                    if (ImGui::Selectable(projectionTypeStrings[i], isSelected)) {
                        camera.Projection = (Camera::ProjectionType)i;
                        camera.RecalculateProjection();
                    }
                    if (isSelected) {
                        ImGui::SetItemDefaultFocus();
                    }
                }
                ImGui::EndCombo();
            }

            // Perspective settings
            if (camera.Projection == Camera::ProjectionType::Perspective) {
                bool changed = false;
                changed |= ImGui::DragFloat("Vertical FOV", &camera.VerticalFOV, 0.1f, 1.0f, 180.0f);
                changed |= ImGui::DragFloat("Near Clip", &camera.NearClip, 0.01f, 0.01f, camera.FarClip);
                changed |= ImGui::DragFloat("Far Clip", &camera.FarClip, 0.1f, camera.NearClip, 10000.0f);

                if (changed) camera.RecalculateProjection();
            }
            // Orthographic settings
            else {
                bool changed = false;
                changed |= ImGui::DragFloat("Size", &camera.OrthographicSize, 0.1f, 0.1f, 100.0f);
                changed |= ImGui::DragFloat("Near", &camera.OrthographicNear, 0.01f);
                changed |= ImGui::DragFloat("Far", &camera.OrthographicFar, 0.01f);

                if (changed) camera.RecalculateProjection();
            }

            // Aspect ratio (could be auto-calculated from viewport)
            ImGui::DragFloat("Aspect Ratio", &camera.AspectRatio, 0.01f, 0.1f, 10.0f);
        });

        DrawComponent<MeshRenderer>("Mesh Renderer", m_SelectedEntity, [](Entity entity, MeshRenderer& meshRenderer) {
            // Model Selection
            ImGui::Text("Mesh");
            ImGui::SameLine();

            // Model UUID drag target
            if (ImGui::Button(meshRenderer.modelNamePreview.empty() ?
                "Drop Model Here" : meshRenderer.modelNamePreview.c_str()))
            {
                // TODO: Open model selection window
            }

            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_UUID")) {
                    const UUID* droppedUUID = static_cast<const UUID*>(payload->Data);
                    if (auto model = ModelLibrary::Get(*droppedUUID)) {
                        meshRenderer.ModelUUID = *droppedUUID;
                        meshRenderer.modelNamePreview = model->GetName();
                    }
                }
                ImGui::EndDragDropTarget();
            }
            ImGui::SameLine();

            // Mesh Index Selection
            if (auto model = ModelLibrary::Get(meshRenderer.ModelUUID)) {
                const uint32_t meshCount = model->GetMeshes().size();
                ImGui::Text("#");
                ImGui::SameLine();
                ImGui::SetNextItemWidth(20);
                ImGui::DragInt("##MeshIndex", reinterpret_cast<int*>(&meshRenderer.MeshIndex), 1.0f, 0, meshCount - 1);
            }

            // Material Selection
            ImGui::Text("Material");
            ImGui::SameLine();

            if (ImGui::Button(meshRenderer.materialNamePreview.empty() ?
                "Drop Material Here" : meshRenderer.materialNamePreview.c_str()))
            {
                // TODO: Open material selection window
            }

            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_UUID")) {
                    const UUID droppedUUID = *static_cast<const UUID*>(payload->Data);
                    if (auto material = MaterialLibrary::Get(droppedUUID)) {
                        meshRenderer.MaterialUUID = droppedUUID;
                        meshRenderer.materialNamePreview = material->GetName();
                        ResourceDB::SetDirty(meshRenderer.ModelUUID);
                        ModelLibrary::Get(meshRenderer.ModelUUID)->AddMaterial(droppedUUID, meshRenderer.MeshIndex);
                    }
                }
                ImGui::EndDragDropTarget();
            }
        });

        // Add Component button
        ImGui::Separator();
        ImGui::Dummy({ 0, 4 });
        AlignItemToCenter(100);
        ButtonDropdown("Add Component", "inspector_addcomponent", [this]() {
            #if defined(DEBUG)
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
            #endif
            if (!m_SelectedEntity.HasComponent<Camera>() && ImGui::MenuItem("Camera")) {
                m_SelectedEntity.AddOrReplaceComponent<Camera>();
                ImGui::CloseCurrentPopup();
            }
            if (!m_SelectedEntity.HasComponent<MeshRenderer>() && ImGui::MenuItem("Mesh Renderer")) {
                m_SelectedEntity.AddOrReplaceComponent<MeshRenderer>();
                ImGui::CloseCurrentPopup();
            }
            // Add more components here as needed
        });
    }

    void InspectorPanel::DrawResourceProperties()
    {
        // Material Properties Visualization
        if (auto material = MaterialLibrary::Get(m_SelectedResource)) {
            // Material header with name and type
            ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.8f, 1.0f), "%s (Material)", material->GetName().c_str());
            ImGui::Dummy({ 0, 4 });
            ImGui::Separator();
            ImGui::Dummy({ 0, 4 });

            // Shader selection
            ImGui::Text("Shader");
            ImGui::SameLine();
            if (auto shader = material->GetShader()) {
                ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
                if (ImGui::BeginCombo("##Shader", shader->GetName().c_str())) {
                    for (const auto& [uuid, s] : ShaderLibrary::GetAllShaders()) {
                        bool selected;
                        if (ImGui::Selectable(s.Shader->GetName().c_str(), &selected)) {
                            material->SetShaderUUID(uuid);
                        }
                    }
                    ImGui::EndCombo();
                }
            }
            else {
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Missing Shader");
            }

            // Render mode
            const char* renderModes[] = { "Opaque", "Cutout", "Transparent", "Fade" };
            static int currentRenderMode = 0;
            ImGui::Text("Render Mode");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
            ImGui::Combo("##RenderMode", &currentRenderMode, renderModes, IM_ARRAYSIZE(renderModes));

            ImGui::Dummy({ 0, 4 });

            // Texture properties with toggle buttons
            const auto& textures = material->GetTextures();

            auto DrawTextureProperty = [&](TextureType type, const char* label) {
                std::shared_ptr<Texture> texture;
                bool hasTexture = false;
                for (const auto& texInfo : textures) {
                    if (texInfo.type == type) {
                        if (texture = TextureCache::Get(texInfo.Uuid)) {
                            hasTexture = true;
                            break;
                        }
                    }
                }

                // Toggle button
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
                std::string toggleId = "##Toggle_" + std::string(label);
                ImGui::Checkbox(toggleId.c_str(), &hasTexture);
                ImGui::SameLine();
                ImGui::Text(label);

                ImGui::Indent();

                // Texture slot with drag-drop support
                if (hasTexture) {
                    ImGui::ImageButton(label, (ImTextureID)texture->GetRendererID(), { 32, 32 }, { 0, 1 }, { 1, 0 });
                }
                else {
                    std::string buttonId = "##Button_" + std::string(label);
                    ImGui::Button(buttonId.c_str(), { 32, 32 });
                }
                ImGui::PopStyleVar();

                if (ImGui::BeginDragDropTarget()) {
                    if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_UUID")) {
                        const UUID* droppedUUID = static_cast<const UUID*>(payload->Data);
                        material->SetTexture({ *droppedUUID, type, 0 });
                        ResourceDB::SetDirty(material->GetUUID());
                    }
                    ImGui::EndDragDropTarget();
                }

                // Texture properties
                if (hasTexture) {
                    ImGui::SameLine();
                    ImGui::BeginGroup();
                    ImGui::Text("%s", texture->GetName().c_str());
                    ImGui::Text("%dx%d", texture->GetWidth(), texture->GetHeight());
                    ImGui::EndGroup();
                }
                ImGui::Unindent();
                ImGui::Spacing();
            };

            DrawTextureProperty(TextureType::Diffuse,   "Albedo");
            DrawTextureProperty(TextureType::Alpha,     "Alpha");
            DrawTextureProperty(TextureType::Normal,    "Normal");
            DrawTextureProperty(TextureType::Metalness, "Metallic");
            DrawTextureProperty(TextureType::Roughness, "Roughness");
            DrawTextureProperty(TextureType::Specular,  "Specular");
            DrawTextureProperty(TextureType::Emissive,  "Emissive");
            DrawTextureProperty(TextureType::Oclusion,  "Oclusion");

            // TODO: Could add color properties, sliders, etc. for each texture channel (too lazy :3)
        }
    }

    template<typename T, typename UIFunction>
    void InspectorPanel::DrawComponent(const std::string& name, Entity entity, UIFunction uiFunction)
    {
        const ImGuiTreeNodeFlags treeNodeFlags = ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_Framed
            | ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_AllowItemOverlap | ImGuiTreeNodeFlags_FramePadding;

        if (entity.HasComponent<T>()) {
            bool open = ImGui::TreeNodeEx(name.c_str(), treeNodeFlags);

            // Right-click to open context menu
            std::string popId = "##Pop_" + name;
            if (ImGui::BeginPopupContextItem(popId.c_str())) {
                constexpr bool enabled = (std::is_same_v<T, Transform> || std::is_same_v<T, ID>) ? false : true;
                if (ImGui::MenuItem("Remove component", nullptr, nullptr, enabled)) {
                    entity.RemoveComponent<T>();
                }
                ImGui::EndPopup();
            }

            if (open) {
                uiFunction(entity, entity.GetComponent<T>());
                ImGui::TreePop();
            }
            
            ImGui::Dummy({ 0, 4 });
        }
    }
}
