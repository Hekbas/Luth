#include "luthpch.h"
#include "luth/editor/panels/InspectorPanel.h"
#include "luth/editor/UI.h"
#include "luth/scene/Components.h"
#include "luth/resources/AssetDatabase.h"
#include "luth/resources/AssetManager.h"
#include "luth/renderer/Model.h"
#include "luth/renderer/Material.h"
#include "luth/renderer/Texture.h"
#include "luth/renderer/Shader.h"
#include "luth/resources/FileSystem.h"
#include "luth/utils/ImGuiUtils.h"
#include "luth/utils/LuthIcons.h"

namespace Luth
{
    using namespace Component;

    InspectorPanel::InspectorPanel()
    {
        LH_CORE_INFO("Created Inspector panel");
    }

    void InspectorPanel::OnInit() {}

    void InspectorPanel::OnRender()
    {
        ImGui::PushFont(Editor::GetFASolid());
        std::string inspector = ICON_FA_CIRCLE_INFO + std::string("  Inspector");

        if (ImGui::Begin(inspector.c_str()))
        {
            Entity selectedEntity = EditorSelection::GetSelectedEntity();
            UUID selectedResource = EditorSelection::GetSelectedResource();

            if (selectedEntity) {
                DrawEntityComponents(selectedEntity);
            }
            else if (selectedResource.IsValid()) {
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

            // Create a horizontal group for checkbox + name
            ImGui::BeginGroup();

            // Checkbox for active state
            bool isActive = m_SelectedEntity.IsActive();
            if (ImGui::Checkbox("##Active", &isActive)) {
                m_SelectedEntity.SetActive(isActive);
                Editor::MarkDirty();
            }

            // Tooltip and spacing
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Toggle Entity Active State");
            }
            ImGui::SameLine();

            // Name field
            char buffer[256];
            strncpy(buffer, tag.m_Tag.c_str(), sizeof(buffer) - 1);
            buffer[sizeof(buffer) - 1] = 0;
            ImGui::PushItemWidth(-1);
            if (ImGui::InputText("##Name", buffer, sizeof(buffer))) {
                tag.m_Tag = std::string(buffer);
                Editor::MarkDirty();
            }

            ImGui::EndGroup();
            ImGui::Dummy({ 0, 4 });
        }

        // Track the active material for the entity inspector
        UUID activeMaterialUUID = UUID::Invalid();

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

        DrawComponent<WorldTransform>("World Transform", m_SelectedEntity, [](Entity entity, WorldTransform& transform) {});
        #endif

        DrawComponent<Transform>("Transform", m_SelectedEntity, [](Entity entity, Transform& transform) {
            if (UI::BeginProperties()) {
                if (UI::Property("Position", transform.Position)) { transform.IsDirty = true; Editor::MarkDirty(); }
                if (UI::Property("Rotation", transform.Rotation)) { transform.IsDirty = true; Editor::MarkDirty(); }
                if (UI::Property("Scale", transform.Scale, 0.1f, 1.0f)) { transform.IsDirty = true; Editor::MarkDirty(); }
                UI::EndProperties();
            }
        });

        DrawComponent<Camera>("Camera", m_SelectedEntity, [](Entity e, Camera& camera) {
            // Projection type combo box
            const char* projectionTypeStrings[] = { "Perspective", "Orthographic" };
            const char* currentProjectionType = projectionTypeStrings[(int)camera.Projection];

            ImGui::Text("Projection"); ImGui::SameLine();
            if (ImGui::BeginCombo("##Projection", currentProjectionType)) {
                for (int i = 0; i < 2; i++) {
                    bool isSelected = currentProjectionType == projectionTypeStrings[i];
                    if (ImGui::Selectable(projectionTypeStrings[i], isSelected)) {
                        camera.Projection = (Camera::ProjectionType)i;
                        camera.IsDirty = true;
                        Editor::MarkDirty();
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
                ImGui::Text("FOV "); ImGui::SameLine();
                changed |= ImGui::DragFloat("##FOV", &camera.VerticalFOV, 0.1f, 1.0f, 180.0f);
                ImGui::Text("Near"); ImGui::SameLine();
                changed |= ImGui::DragFloat("##Near", &camera.NearClip, 0.01f, 0.01f, camera.FarClip);
                ImGui::Text("Far "); ImGui::SameLine();
                changed |= ImGui::DragFloat("##Far", &camera.FarClip, 0.1f, camera.NearClip, 10000.0f);

                if (changed) { camera.IsDirty = true; Editor::MarkDirty(); }
            }
            // Orthographic settings
            else {
                bool changed = false;
                ImGui::Text("Size"); ImGui::SameLine();
                changed |= ImGui::DragFloat("##Size", &camera.OrthographicSize, 0.1f, 0.1f, 100.0f);
                ImGui::Text("Near"); ImGui::SameLine();
                changed |= ImGui::DragFloat("##Near", &camera.OrthographicNear, 0.01f);
                ImGui::Text("Far "); ImGui::SameLine();
                changed |= ImGui::DragFloat("##Far", &camera.OrthographicFar, 0.01f);

                if (changed) { camera.IsDirty = true; Editor::MarkDirty(); }
            }

            // Aspect ratio (could be auto-calculated from viewport)
            ImGui::Text("Aspect"); ImGui::SameLine();
            if (ImGui::DragFloat("##Aspect", &camera.AspectRatio, 0.01f, 0.1f, 10.0f)) { camera.IsDirty = true; Editor::MarkDirty(); }
        });

        DrawComponent<MeshRenderer>("Mesh Renderer", m_SelectedEntity, [&](Entity entity, MeshRenderer& meshRenderer) {
            if (UI::BeginProperties()) {

                if (UI::PropertyAsset("Model", meshRenderer.ModelUUID, AssetType::Model))
                    Editor::MarkDirty();

                // Ensure model is loaded to get mesh count
                if (meshRenderer.ModelUUID.IsValid() && !AssetManager::IsLoaded(meshRenderer.ModelUUID) && !AssetManager::IsLoading(meshRenderer.ModelUUID))
                     AssetManager::LoadAsync(meshRenderer.ModelUUID);

                if (auto model = AssetManager::GetAsset<Model>(meshRenderer.ModelUUID)) {
                    int meshIndex = (int)meshRenderer.MeshIndex;
                    if (UI::Property("Mesh Index", meshIndex, 0, (int)model->GetMeshes().size() - 1)) {
                        meshRenderer.MeshIndex = (u32)meshIndex;
                        Editor::MarkDirty();
                    }
                }

                if (UI::PropertyAsset("Material", meshRenderer.MaterialUUID, AssetType::Material))
                {
                    // Update model's material list if possible
                    if (auto model = AssetManager::GetAsset<Model>(meshRenderer.ModelUUID))
                    {
                        model->AddMaterial(meshRenderer.MaterialUUID, meshRenderer.MeshIndex);
                    }
                    Editor::MarkDirty();
                }

                UI::EndProperties();
            }

            activeMaterialUUID = meshRenderer.MaterialUUID;
        });

        DrawComponent<Animation>("Animation", m_SelectedEntity, [](Entity entity, Animation& animation) {
			// TODO: Implement animation component properties
			if (ImGui::SliderInt("##Animation Index", &animation.AnimationIndex, 0, 20, "Index: %d", ImGuiSliderFlags_AlwaysClamp))
                Editor::MarkDirty();
        });

        DrawComponent<DirectionalLight>("Directional Light", m_SelectedEntity, [](Entity entity, DirectionalLight& dirLight) {
            bool changed = false;
            if (UI::BeginProperties()) {
                changed |= UI::PropertyColor("Color", dirLight.Color);
                changed |= UI::Property("Intensity", dirLight.Intensity, 0.1f, 0.0f, 1000.0f);
                changed |= UI::Property("Cast Shadows", dirLight.CastShadows);
                if (dirLight.CastShadows) {
                    changed |= UI::Property("Shadow Bias", dirLight.ShadowBias, 0.0001f, 0.0f, 0.05f);
                    changed |= UI::Property("Shadow Size", dirLight.ShadowOrthoSize, 1.0f, 10.0f, 2000.0f);
                    changed |= UI::Property("Shadow Distance", dirLight.ShadowDistance, 1.0f, 10.0f, 2000.0f);
                }
                UI::EndProperties();
            }
            if (changed) Editor::MarkDirty();
        });

        DrawComponent<PointLight>("Point Light", m_SelectedEntity, [](Entity entity, PointLight& pointLight) {
            bool changed = false;
            if (UI::BeginProperties()) {
                changed |= UI::PropertyColor("Color", pointLight.Color);
                changed |= UI::Property("Intensity", pointLight.Intensity, 0.1f, 0.0f, 1000.0f);
                changed |= UI::Property("Range", pointLight.Range, 0.1f, 0.0f, 10000.0f);
                UI::EndProperties();
            }
            if (changed) Editor::MarkDirty();
        });

        // Add Component button
        ImGui::Separator();
        ImGui::Dummy({ 0, 4 });
        AlignItemToCenter(100);
        ButtonDropdown("Add Component", "inspector_addcomponent", [&m_SelectedEntity]() {
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
            if (!m_SelectedEntity.HasComponent<DirectionalLight>() && ImGui::MenuItem("Directional Light")) {
                m_SelectedEntity.AddOrReplaceComponent<DirectionalLight>();
                ImGui::CloseCurrentPopup();
            }
            if (!m_SelectedEntity.HasComponent<PointLight>() && ImGui::MenuItem("Point Light")) {
                m_SelectedEntity.AddOrReplaceComponent<PointLight>();
                ImGui::CloseCurrentPopup();
            }
            if (!m_SelectedEntity.HasComponent<MeshRenderer>() && ImGui::MenuItem("Mesh Renderer")) {
                m_SelectedEntity.AddOrReplaceComponent<MeshRenderer>();
                ImGui::CloseCurrentPopup();
            }
            if (!m_SelectedEntity.HasComponent<Animation>() && ImGui::MenuItem("Animation")) {
                m_SelectedEntity.AddOrReplaceComponent<Animation>();
                ImGui::CloseCurrentPopup();
            }
        });

        if (activeMaterialUUID.IsValid()) {
            if (!AssetManager::IsLoaded(activeMaterialUUID) && !AssetManager::IsLoading(activeMaterialUUID))
                AssetManager::LoadAsync(activeMaterialUUID);

            if (auto mat = AssetManager::GetAsset<Material>(activeMaterialUUID)) {
                ImGui::Dummy({ 0, 8 });
                ImGui::Separator();
                ImGui::Dummy({ 0, 4 });
                m_MaterialEditor.Draw(*mat);
            }
        }
    }

    template<typename T, typename UIFunction>
    void InspectorPanel::DrawComponent(const std::string& name, Entity entity, UIFunction uiFunction)
    {
        if (entity.HasComponent<T>()) {
            auto contextMenu = [&]() {
                constexpr bool enabled = (std::is_same_v<T, Transform> || std::is_same_v<T, ID>) ? false : true;
                if (ImGui::MenuItem("Remove component", nullptr, nullptr, enabled)) {
                    entity.RemoveComponent<T>();
                }
            };

            bool open = UI::BeginCollapsingHeader(name.c_str(), true, contextMenu);

            if (open) {
                uiFunction(entity, entity.GetComponent<T>());
                UI::EndCollapsingHeader();
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
