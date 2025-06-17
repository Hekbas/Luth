#include "luthpch.h"
#include "luth/editor/panels/InspectorPanel.h"
#include "luth/editor/panels/HierarchyPanel.h"
#include "luth/ECS/Components.h"
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

        DrawComponent<WorldTransform>("World Transform", m_SelectedEntity, [](Entity entity, WorldTransform& transform) {});
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
            ImGui::Text("Scale"); ImGui::SameLine();
            
            // Lock toggle button (using padlock icon)
            static bool scaleLocked = false;
            ImGui::Checkbox("##ScaleLock", &scaleLocked);
            ImGui::SameLine();
            if (scaleLocked) {
                // Uniform scale control
                float uniformScale = transform.m_Scale.x;
                if (ImGui::DragFloat("##UniformScale", &uniformScale, 0.1f)) {
                    transform.m_Scale = glm::vec3(uniformScale);
                }
            }
            else {
                // Independent axis control
                if (ImGui::DragFloat3("##Scale", glm::value_ptr(transform.m_Scale), 0.1f)) {
                    // Optional: Sync scale if any component was changed to zero
                    if (transform.m_Scale.x == 0 || transform.m_Scale.y == 0 || transform.m_Scale.z == 0) {
                        transform.m_Scale = glm::max(transform.m_Scale, glm::vec3(0.001f));
                    }
                }
            }

            // Reset buttons
            if (ImGui::Button("Reset Transform")) {
                transform.m_Position = { 0,0,0 };
                transform.m_Rotation = { 0,0,0 };
                transform.m_Scale    = { 1,1,1 };
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
                ImGui::Text("FOV "); ImGui::SameLine();
                changed |= ImGui::DragFloat("##FOV", &camera.VerticalFOV, 0.1f, 1.0f, 180.0f);
                ImGui::Text("Near"); ImGui::SameLine();
                changed |= ImGui::DragFloat("##Near", &camera.NearClip, 0.01f, 0.01f, camera.FarClip);
                ImGui::Text("Far "); ImGui::SameLine();
                changed |= ImGui::DragFloat("##Far", &camera.FarClip, 0.1f, camera.NearClip, 10000.0f);

                if (changed) camera.RecalculateProjection();
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

                if (changed) camera.RecalculateProjection();
            }

            // Aspect ratio (could be auto-calculated from viewport)
            ImGui::Text("Aspect"); ImGui::SameLine();
            ImGui::DragFloat("##Aspect", &camera.AspectRatio, 0.01f, 0.1f, 10.0f);
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

        DrawComponent<Animation>("Animation", m_SelectedEntity, [](Entity entity, Animation& animation) {
			// TODO: Implement animation component properties
			ImGui::SliderInt("##Animation Index", &animation.AnimationIndex, 0, 20, "Index: %d", ImGuiSliderFlags_AlwaysClamp);
        });

        DrawComponent<DirectionalLight>("Directional Light", m_SelectedEntity, [](Entity entity, DirectionalLight& dirLight) {
            ImGui::Text("Color"); ImGui::SameLine();
            ImGui::ColorEdit3("##Color", &dirLight.Color.x);
            ImGui::Text("Intensity"); ImGui::SameLine();
            ImGui::DragFloat("##Intensity", &dirLight.Intensity, 0.01f, 0.0f, 1000.0f);
        });

        DrawComponent<PointLight>("Point Light", m_SelectedEntity, [](Entity entity, PointLight& pointLight) {
            ImGui::Text("Color"); ImGui::SameLine();
            ImGui::ColorEdit3("##Color", &pointLight.Color.x);
            ImGui::Text("Intensity"); ImGui::SameLine();
            ImGui::DragFloat("##Intensity", &pointLight.Intensity, 0.01f, 0.0f, 1000.0f);
            ImGui::Text("Range"); ImGui::SameLine();
            ImGui::DragFloat("##Range", &pointLight.Range, 0.1f, 0.0f, 10000.0f);
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
            if (!m_SelectedEntity.HasComponent<DirectionalLight>() && ImGui::MenuItem("Directional Light")) {
                m_SelectedEntity.AddOrReplaceComponent<DirectionalLight>();
                ImGui::CloseCurrentPopup();
            }
            if (!m_SelectedEntity.HasComponent<PointLight>() && ImGui::MenuItem("Point Light")) {
                m_SelectedEntity.AddOrReplaceComponent<PointLight>();
                ImGui::CloseCurrentPopup();
            }
            // Add more components here as needed
        });
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
    
    void InspectorPanel::DrawResourceProperties()
    {
        if (auto model = ModelLibrary::Get(m_SelectedResource)) {
            DrawModel(*model);
        }
        else if (auto material = MaterialLibrary::Get(m_SelectedResource)) {
			DrawMaterial(*material);
        }
        else if (auto texture = TextureCache::Get(m_SelectedResource)) {
            DrawTexture(*texture);
        }
    }

    void InspectorPanel::DrawModel(Model& model)
    {
        // Model header with name and type
        if (ImGui::BeginChild("##Header", { 0, 30 })) {
			ImGui::Dummy({ 0, 4 }); ImGui::Dummy({ 4, 0 }); ImGui::SameLine();
            ImGui::TextColored({ 0.4f, 0.8f, 1.0f, 1.0f }, "%s (Model)", model.GetName().c_str());
        }
        ImGui::EndChild();
        ImGui::Dummy({ 0, 8 });

        // Get cached model info
        const auto& info = model.GetCachedModelInfo();

        // Basic model info section
        if (ImGui::CollapsingHeader("Model Info", ImGuiTreeNodeFlags_DefaultOpen)) {
            // Basic counts
            if (ImGui::BeginTable("ModelProps", 2, ImGuiTableFlags_SizingStretchSame)) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::Text("Meshes");
                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%d", info.TotalMeshCount);

                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::Text("Vertices");
                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%d", info.TotalVertexCount);

                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::Text("Indices");
                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%d", info.TotalIndexCount);

                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::Text("Materials");
                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%d", info.MaterialCount);

                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::Text("Skinned");
                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%s", info.IsSkinned ? "Yes" : "No");

                ImGui::EndTable();
            }
        }
        ImGui::Dummy({ 0, 4 });

        // Meshes section
        if (ImGui::CollapsingHeader("Meshes")) {
            if (ImGui::BeginTable("MeshesTable", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
                ImGui::TableSetupColumn("Name");
                ImGui::TableSetupColumn("Vertices");
                ImGui::TableSetupColumn("Indices");
                ImGui::TableSetupColumn("Material");
                ImGui::TableHeadersRow();

                for (const auto& mesh : info.Meshes) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::Text("%s", mesh.Name.c_str());
                    ImGui::TableNextColumn();
                    ImGui::Text("%d", mesh.VertexCount);
                    ImGui::TableNextColumn();
                    ImGui::Text("%d", mesh.IndexCount);
                    ImGui::TableNextColumn();
                    ImGui::Text("%d", mesh.MaterialIndex);
                }

                ImGui::EndTable();
            }
        }
        ImGui::Dummy({ 0, 4 });

        // Skinned model specific sections
        if (info.IsSkinned) {
            // Bones section
            if (ImGui::CollapsingHeader("Bones")) {
                ImGui::Text("Total Bones: %d", info.BoneCount);

                if (ImGui::TreeNode("Bone Hierarchy")) {
                    // Recursive function to display bone hierarchy
                    std::function<void(int)> DisplayBoneNode = [&](int index) {
                        const auto& node = info.BoneHierarchy[index];
                        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow;
                        if (node.BoneIndex == -1) flags |= ImGuiTreeNodeFlags_Leaf;

                        bool isOpen = ImGui::TreeNodeEx(node.Name.c_str(), flags);

                        // Tooltip for bone info
                        if (ImGui::IsItemHovered()) {
                            ImGui::BeginTooltip();
                            ImGui::Text("Bone Index: %d", node.BoneIndex);
                            ImGui::Text("Parent Index: %d", node.ParentIndex);
                            ImGui::EndTooltip();
                        }

                        if (isOpen) {
                            for (int childIndex = 0; childIndex < info.BoneHierarchy.size(); ++childIndex) {
                                if (info.BoneHierarchy[childIndex].ParentIndex == index) {
                                    DisplayBoneNode(childIndex);
                                }
                            }
                            ImGui::TreePop();
                        }
                    };

                    // Find root nodes (parentIndex == -1)
                    for (int i = 0; i < info.BoneHierarchy.size(); ++i) {
                        if (info.BoneHierarchy[i].ParentIndex == -1) {
                            DisplayBoneNode(i);
                        }
                    }

                    ImGui::TreePop();
                }
            }
            ImGui::Dummy({ 0, 4 });

            // Animations section
            if (ImGui::CollapsingHeader("Animations")) {
                ImGui::Text("Total Animations: %d", info.AnimationCount);

                if (ImGui::BeginTable("AnimationsTable", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
                    ImGui::TableSetupColumn("Name");
                    ImGui::TableSetupColumn("Duration");
                    ImGui::TableSetupColumn("TPS");
                    ImGui::TableHeadersRow();

                    for (const auto& anim : info.Animations) {
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::Text("%s", anim.Name.c_str());
                        ImGui::TableNextColumn();
                        ImGui::Text("%.2f", anim.Duration);
                        ImGui::TableNextColumn();
                        ImGui::Text("%.2f", anim.TicksPerSecond);
                    }

                    ImGui::EndTable();
                }
            }
        }
    }

    void InspectorPanel::DrawMaterial(Material& material)
    {
        // Material header with name and type
        if (ImGui::BeginChild("##Header", { 0, 30 })) {
            ImGui::Dummy({ 0, 4 }); ImGui::Dummy({ 4, 0 }); ImGui::SameLine();
            ImGui::TextColored({ 0.2f, 0.9f, 0.4f, 1.0f }, "%s (Material)", material.GetName().c_str());
        }
        ImGui::EndChild();
        ImGui::Dummy({ 0, 8 });

        // Shader selection
        ImGui::Text("Shader     ");
        ImGui::SameLine();
        if (auto shader = material.GetShader()) {
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
            if (ImGui::BeginCombo("##Shader", shader->GetName().c_str())) {
                for (const auto& [uuid, s] : ShaderLibrary::GetAllShaders()) {
                    bool selected;
                    if (ImGui::Selectable(s.Shader->GetName().c_str(), &selected)) {
                        material.SetShaderUUID(uuid);
                        ResourceDB::SetDirty(material.GetUUID());
                    }
                }
                ImGui::EndCombo();
            }
        }
        else {
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Missing Shader");
        }

        // Render mode
        RendererAPI::RenderMode currentMode = material.GetRenderMode();
        int modeIndex = static_cast<int>(currentMode);

        ImGui::Text("Render Mode"); ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
        const char* renderModes[] = { "Opaque", "Cutout", "Transparent", "Fade" };
        if (ImGui::Combo("##RenderMode", &modeIndex, renderModes, IM_ARRAYSIZE(renderModes))) {
            material.SetRenderMode(static_cast<RendererAPI::RenderMode>(modeIndex));
            ResourceDB::SetDirty(material.GetUUID());
        }

        if (material.GetRenderMode() == RendererAPI::RenderMode::Cutout) {
            float cutoff = material.GetAlphaCutoff();
            ImGui::Text("Alpha Cutoff"); ImGui::SameLine();
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
            if (ImGui::SliderFloat("##Alpha Cutoff", &cutoff, 0.0f, 1.0f)) {
                material.SetAlphaCutoff(cutoff);
                ResourceDB::SetDirty(material.GetUUID());
            }
        }

        if (material.GetRenderMode() == RendererAPI::RenderMode::Transparent ||
            material.GetRenderMode() == RendererAPI::RenderMode::Fade)
        {
            int srcFactor = static_cast<int>(material.GetBlendSrc());
            int dstFactor = static_cast<int>(material.GetBlendDst());

            const char* blendFactors[] = { "Zero", "One", "SrcAlpha", "OneMinusSrcAlpha" };

            ImGui::Text("Blend Src  "); ImGui::SameLine();
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
            if (ImGui::Combo("##Blend Src", &srcFactor, blendFactors, IM_ARRAYSIZE(blendFactors))) {
                material.SetBlendSrc(static_cast<RendererAPI::BlendFactor>(srcFactor));
                ResourceDB::SetDirty(material.GetUUID());
            }

            ImGui::Text("Blend Dst  "); ImGui::SameLine();
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
            if (ImGui::Combo("##Blend Dst", &dstFactor, blendFactors, IM_ARRAYSIZE(blendFactors))) {
                material.SetBlendDst(static_cast<RendererAPI::BlendFactor>(dstFactor));
                ResourceDB::SetDirty(material.GetUUID());
            }

            bool fromDiffuse = material.IsAlphaFromDiffuseEnabled();
            if (ImGui::Checkbox("Alpha from Diffuse", &fromDiffuse)) {
                material.EnableAlphaFromDiffuse(fromDiffuse);
                ResourceDB::SetDirty(material.GetUUID());
            }
        }

        ImGui::Dummy({ 0, 4 });

        // Texture properties with collapsable headers
        const auto& textures = material.GetTextures();

        auto DrawTextureProperty = [&](MapType type, const char* label) {
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

            // Header setup
            ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_Framed |
                ImGuiTreeNodeFlags_AllowItemOverlap |
                ImGuiTreeNodeFlags_NoTreePushOnOpen |
                ImGuiTreeNodeFlags_DefaultOpen;

            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(2, 2));
            bool headerOpen = ImGui::CollapsingHeader(label, flags);

            // Checkbox control
            ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - 12);
            std::string toggleId = "##Toggle_" + std::string(label);
            bool enabled = material.IsUseMapEnabled(type);
            if (ImGui::Checkbox(toggleId.c_str(), &enabled)) {
                material.EnableUseMap(type, enabled);
            }

            ImGui::PopStyleVar();

            if (headerOpen) {
                ImGui::BeginDisabled(!enabled);
                ImGui::Indent();

                // Texture slot with drag-drop support
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
                std::string textureId = "##Texture_" + std::string(label);
                if (hasTexture) {
                    ImGui::ImageButton(textureId.c_str(), (ImTextureID)texture->GetRendererID(), { 32, 32 }, { 0, 1 }, { 1, 0 });
                }
                else {
                    ImGui::Button(textureId.c_str(), { 32, 32 });
                }
                ImGui::PopStyleVar();

                if (ImGui::BeginDragDropTarget()) {
                    if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_UUID")) {
                        const UUID* droppedUUID = static_cast<const UUID*>(payload->Data);
                        material.SetTexture({ *droppedUUID, type, 0 });
                        material.EnableUseTexture(type, true);
                        ResourceDB::SetDirty(material.GetUUID());
                    }
                    ImGui::EndDragDropTarget();
                }

                // [SUPR] Handle texture deletion
                if (ImGui::IsItemActive() && ImGui::IsKeyPressed(ImGuiKey_Delete)) {
                    material.SetTexture({ UUID(3), type, 0 });
                    material.EnableUseTexture(type, false);
                    ResourceDB::SetDirty(material.GetUUID());
                }

                // Texture properties
                if (hasTexture) {
                    ImGui::SameLine();
                    ImGui::BeginGroup();
                    ImGui::Text("%s", texture->GetName().c_str());
                    ImGui::Text("%dx%d", texture->GetWidth(), texture->GetHeight());
                    ImGui::EndGroup();
                }

                // Texture specific properties
                if (type == MapType::Diffuse) {
                    ImGui::SameLine();

                    Vec4 color = material.GetColor();
                    if (ImGui::ColorEdit4("##DiffuseColor", &color.r, ImGuiColorEditFlags_NoInputs |
                        ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_AlphaPreview)) {
                        material.SetColor(color);
                        ResourceDB::SetDirty(material.GetUUID());
                    }

                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("Diffuse Color");
                    }
                }
                else if (type == MapType::Alpha) {
                    float alpha = material.GetAlpha();
                    if (ImGui::SliderFloat("##Alpha", &alpha, 0.0f, 1.0f)) {
                        material.SetAlpha(alpha);
                        ResourceDB::SetDirty(material.GetUUID());
                    }
                }
                else if (type == MapType::Metalness) {
                    float metal = material.GetMetal();
                    if (ImGui::SliderFloat("##Metalness", &metal, 0.0f, 1.0f)) {
                        material.SetMetal(metal);
                        ResourceDB::SetDirty(material.GetUUID());
                    }
                }
                else if (type == MapType::Roughness) {
                    float rough = material.GetRough();
                    if (ImGui::SliderFloat("##Roughness", &rough, 0.0f, 1.0f)) {
                        material.SetRough(rough);
                        ResourceDB::SetDirty(material.GetUUID());
                    }
                    bool isGloss = material.IsGloss();
                    if (ImGui::Checkbox("Is Gloss", &isGloss)) {
                        material.SetGloss(isGloss);
                        ResourceDB::SetDirty(material.GetUUID());
                    }
                }
                else if (type == MapType::Emissive) {
                    ImGui::SameLine();

                    Vec3 emissive = material.GetEmissive();
                    if (ImGui::ColorEdit3("##EmissiveColor", &emissive.r, ImGuiColorEditFlags_NoInputs)) {
                        material.SetEmissive(emissive);
                        ResourceDB::SetDirty(material.GetUUID());
                    }

                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("Emissive Color");
                    }

                    bool isSingle = material.IsSingleChannel();
                    if (ImGui::Checkbox("Single Channel", &isSingle)) {
                        material.SetSingleChannel(isSingle);
                        ResourceDB::SetDirty(material.GetUUID());
                    }
                }
                else if (type == MapType::Thickness) {
                    ImGui::SameLine();

                    Vec3 thick = material.GetSubsurface().color;
                    if (ImGui::ColorEdit3("##ThicknessColor", &thick.r, ImGuiColorEditFlags_NoInputs)) {
                        material.SetSubsurfaceColor(thick);
                        ResourceDB::SetDirty(material.GetUUID());
                    }

                    float strength = material.GetSubsurface().strength;
                    if (ImGui::SliderFloat("##ThicknessStrength", &strength, 0.0f, 1.0f)) {
                        material.SetSubsurfaceStrength(strength);
                        ResourceDB::SetDirty(material.GetUUID());
                    }

                    float scale = material.GetSubsurface().thicknessScale;
                    if (ImGui::SliderFloat("##ThicknessScale", &scale, 0.0f, 1.0f)) {
                        material.SetSubsurfaceThicknessScale(scale);
                        ResourceDB::SetDirty(material.GetUUID());
                    }
                }

                ImGui::Unindent();
                ImGui::EndDisabled();
            }
            ImGui::Spacing();
            };

        DrawTextureProperty(MapType::Diffuse, "Albedo");
        DrawTextureProperty(MapType::Alpha, "Alpha");
        DrawTextureProperty(MapType::Normal, "Normal");
        DrawTextureProperty(MapType::Metalness, "Metallic");
        DrawTextureProperty(MapType::Roughness, "Roughness");
        DrawTextureProperty(MapType::Specular, "Specular");
        DrawTextureProperty(MapType::Oclusion, "Oclusion");
        DrawTextureProperty(MapType::Emissive, "Emissive");
        DrawTextureProperty(MapType::Thickness, "Thickness");
    }

    void InspectorPanel::DrawTexture(Texture& texture)
    {
        // Texture header with name and type
        if (ImGui::BeginChild("##Header", { 0, 30 })) {
            ImGui::Dummy({ 0, 4 }); ImGui::Dummy({ 4, 0 }); ImGui::SameLine();
            ImGui::TextColored({ 0.8f, 0.6f, 0.2f, 1.0f }, "%s (Texture)", texture.GetName().c_str());
        }
        ImGui::EndChild();
        ImGui::Dummy({ 0, 8 });

        // Properties table
        static int wrapMode = (int)texture.GetWrapMode();
        const char* wrapModes[] = { "Repeat", "Clamp to Edge", "Mirrored Repeat" };

        static int minFilter = (int)texture.GetFilterMode().first;
        static int magFilter = (int)texture.GetFilterMode().second;
        const char* filterModes[] = { "Nearest", "Linear", "Linear Mipmap", "Nearest Mipmap" };

        if (ImGui::BeginTable("TextureProps", 2, ImGuiTableFlags_SizingStretchSame)) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("Dimensions");
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%d x %d", texture.GetWidth(), texture.GetHeight());

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("Format");
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%s", texture.GetFormatString());

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("Type");
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%s", "2D");

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("Mip Levels");
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%d", texture.GetMipLevels());
            ImGui::Dummy({ 0, 8 });

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("Wrap Mode");
            ImGui::TableSetColumnIndex(1);
			ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
            ImGui::Combo("##Wrap Mode", &wrapMode, wrapModes, IM_ARRAYSIZE(wrapModes));

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("Min Filter");
            ImGui::TableSetColumnIndex(1);
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
            ImGui::Combo("##Min Filter", &minFilter, filterModes, IM_ARRAYSIZE(filterModes));

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("Mag Filter");
            ImGui::TableSetColumnIndex(1);
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
            ImGui::Combo("##Mag Filter", &magFilter, filterModes, IM_ARRAYSIZE(filterModes));

            ImGui::EndTable();
        }
        ImGui::Dummy({ 0, 8 });

        // Apply button
		ImGui::SetCursorPosX(ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize("Apply").x - ImGui::GetStyle().ItemSpacing.x);
        if (ImGui::Button("Apply")) {
            // TODO: Apply texture settings here
            texture.SetWrapMode((TextureWrapMode)wrapMode);
            //texture.SetFilterMode((TextureFilterMode)filterMode, (TextureFilterMode)filterMode);
        }

        // Padding
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4());
        ImGui::BeginChild("##Padding", { 0, 8 }, ImGuiChildFlags_ResizeY);
        ImGui::EndChild();
        ImGui::PopStyleColor();

        // Preview
        // Calculate preview size
        float imageAR = texture.GetHeight() / texture.GetWidth();
        float availWidth = ImGui::GetContentRegionAvail().x;
        float availHeight = ImGui::GetContentRegionAvail().y;
        float availAR = availHeight / availWidth;
        float previewWidth, previewHeight;
        ImVec2 offset;
		if (availAR > 1.0f) {   // Portrait
			previewWidth = availWidth;
			previewHeight = previewWidth * imageAR;
			offset = { 0, (availHeight - previewHeight) / 2.0f };
		}
		else {  // Landscape
			previewHeight = availHeight;
			previewWidth = previewHeight / imageAR;
			offset = { (availWidth - previewWidth) / 2.0f, 0 };
		}

        // Preview region with texture
        if (ImGui::BeginChild("PreviewRegion", { availWidth, 0 })) {
            ImGui::SetCursorPos(offset);
            ImGui::Image((ImTextureID)(intptr_t)texture.GetRendererID(),
                { previewWidth, previewHeight }, { 0, 1 }, { 1, 0 });
        }
        ImGui::EndChild();
    }
}
