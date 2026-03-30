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
            // Auto-sync ModelUUID from MeshRenderer if not set
            if (!animation.ModelUUID.IsValid() && entity.HasComponent<MeshRenderer>()) {
                auto& mr = entity.GetComponent<MeshRenderer>();
                if (mr.ModelUUID.IsValid())
                    animation.ModelUUID = mr.ModelUUID;
            }

            // Resolve model
            if (!animation.ModelUUID.IsValid()) {
                ImGui::TextDisabled("No model assigned");
                return;
            }
            if (!AssetManager::IsLoaded(animation.ModelUUID)) {
                if (!AssetManager::IsLoading(animation.ModelUUID))
                    AssetManager::LoadAsync(animation.ModelUUID);
                ImGui::TextDisabled("Loading model...");
                return;
            }
            auto model = AssetManager::GetAsset<Model>(animation.ModelUUID);
            if (!model || !model->IsSkinned()) {
                ImGui::TextDisabled("Model has no animations");
                return;
            }

            const auto& clips = model->GetAnimationClips();
            int clipCount = (int)clips.size();
            if (clipCount == 0) {
                ImGui::TextDisabled("No animation clips");
                return;
            }

            // Clip selector
            std::vector<const char*> clipNames(clipCount);
            for (int i = 0; i < clipCount; i++)
                clipNames[i] = clips[i].Name.c_str();

            animation.AnimationIndex = std::clamp(animation.AnimationIndex, 0, clipCount - 1);
            if (ImGui::Combo("Clip##Anim", &animation.AnimationIndex, clipNames.data(), clipCount)) {
                animation.CurrentTime = 0.0f;
                Editor::MarkDirty();
            }

            const AnimationClip* clip = model->GetAnimationClip((u32)animation.AnimationIndex);
            if (!clip) return;
            f32 duration = clip->GetDurationSeconds();

            // Transport controls
            if (animation.Playing) {
                if (ImGui::Button(ICON_FA_PAUSE "##AnimPause"))
                    animation.Playing = false;
            } else {
                if (ImGui::Button(ICON_FA_PLAY "##AnimPlay"))
                    animation.Playing = true;
            }
            ImGui::SameLine();
            if (ImGui::Button(ICON_FA_STOP "##AnimStop")) {
                animation.Playing = false;
                animation.CurrentTime = 0.0f;
            }

            // Speed slider
            if (ImGui::SliderFloat("Speed##Anim", &animation.Speed, 0.0f, 5.0f, "%.2f"))
                Editor::MarkDirty();

            // Loop toggle
            if (ImGui::Checkbox("Loop##Anim", &animation.Loop))
                Editor::MarkDirty();

            // Timeline scrubber
            if (duration > 0.0f) {
                if (ImGui::SliderFloat("Timeline##Anim", &animation.CurrentTime, 0.0f, duration, "%.2f s"))
                    Editor::MarkDirty();
            }

            // Frame counter
            f32 tps = (clip->TicksPerSecond > 0.0f) ? clip->TicksPerSecond : 25.0f;
            int frame = (int)(animation.CurrentTime * tps);
            int totalFrames = (int)clip->Duration;
            ImGui::Text("Frame: %d / %d", frame, totalFrames);

            // Show Bones toggle
            ImGui::Checkbox("Show Bones##AnimDebug", &Editor::GetSettings().showBoneDebug);
        });

        DrawComponent<BoneAttachment>("Bone Attachment", m_SelectedEntity, [](Entity entity, BoneAttachment& attachment) {
            Scene* scene = entity.GetScene();
            if (!scene) return;

            // Target entity combo — list all entities with Animation + Tag
            auto& registry = scene->Registry();
            std::vector<entt::entity> animEntities;
            std::vector<std::string> entityNames;
            int currentIndex = -1;

            auto view = registry.view<Animation, Tag>();
            for (auto e : view) {
                animEntities.push_back(e);
                entityNames.push_back(registry.get<Tag>(e).m_Tag);
                if (attachment.TargetEntity && (entt::entity)attachment.TargetEntity == e)
                    currentIndex = (int)animEntities.size() - 1;
            }

            std::string preview = (currentIndex >= 0) ? entityNames[currentIndex] : "None";
            if (ImGui::BeginCombo("Target##BoneAttach", preview.c_str())) {
                if (ImGui::Selectable("None", currentIndex < 0)) {
                    attachment.TargetEntity = {};
                    attachment.BoneIndex = -1;
                    attachment.BoneName = "";
                    Editor::MarkDirty();
                }
                for (int i = 0; i < (int)animEntities.size(); i++) {
                    bool selected = (i == currentIndex);
                    if (ImGui::Selectable(entityNames[i].c_str(), selected)) {
                        attachment.TargetEntity = Entity(animEntities[i], scene);
                        attachment.BoneIndex = -1;
                        attachment.BoneName = "";
                        Editor::MarkDirty();
                    }
                }
                ImGui::EndCombo();
            }

            // Bone name dropdown
            if (attachment.TargetEntity && attachment.TargetEntity.IsValid()
                && attachment.TargetEntity.HasComponent<Animation>())
            {
                auto& targetAnim = attachment.TargetEntity.GetComponent<Animation>();
                if (auto model = AssetManager::GetAsset<Model>(targetAnim.ModelUUID)) {
                    const auto& skeleton = model->GetSkeleton();
                    if (!skeleton.IsEmpty()) {
                        int boneCount = (int)skeleton.BoneCount();
                        std::vector<const char*> boneNames(boneCount);
                        for (int i = 0; i < boneCount; i++)
                            boneNames[i] = skeleton.Bones[i].Name.c_str();

                        int boneIdx = skeleton.FindBone(attachment.BoneName);
                        if (boneIdx < 0) boneIdx = 0;

                        if (ImGui::Combo("Bone##BoneAttach", &boneIdx, boneNames.data(), boneCount)) {
                            attachment.BoneName = skeleton.Bones[boneIdx].Name;
                            attachment.BoneIndex = -1; // Force re-resolve by AnimationSystem
                            Editor::MarkDirty();
                        }
                    }
                }
            }

            // Local offset and rotation
            if (UI::BeginProperties("BoneAttachProps")) {
                if (UI::Property("Local Offset", attachment.LocalOffset))
                    Editor::MarkDirty();
                if (UI::Property("Local Rotation", attachment.LocalRotation))
                    Editor::MarkDirty();
                UI::EndProperties();
            }
        });

        DrawComponent<AnimationController>("Animation Controller", m_SelectedEntity, [](Entity entity, AnimationController& ctrl) {
            // Require Animation component for model access
            if (!entity.HasComponent<Animation>()) {
                ImGui::TextDisabled("Requires Animation component");
                return;
            }
            auto& anim = entity.GetComponent<Animation>();
            if (!anim.ModelUUID.IsValid()) {
                ImGui::TextDisabled("No model assigned");
                return;
            }
            auto model = AssetManager::GetAsset<Model>(anim.ModelUUID);
            if (!model || !model->IsSkinned()) {
                ImGui::TextDisabled("Model has no animations");
                return;
            }

            const auto& clips = model->GetAnimationClips();
            int clipCount = (int)clips.size();
            if (clipCount == 0) {
                ImGui::TextDisabled("No animation clips");
                return;
            }

            std::vector<const char*> clipNames(clipCount);
            for (int i = 0; i < clipCount; i++)
                clipNames[i] = clips[i].Name.c_str();

            // Current clip selector (base layer)
            int currentClip = ctrl.CurrentClipIndex;
            currentClip = std::clamp(currentClip, 0, clipCount - 1);
            if (ImGui::Combo("Current Clip##Ctrl", &currentClip, clipNames.data(), clipCount)) {
                ctrl.Play(currentClip);
                Editor::MarkDirty();
            }

            // Root motion
            if (ImGui::Checkbox("Root Motion##Ctrl", &ctrl.ApplyRootMotion))
                Editor::MarkDirty();

            // Default transition duration
            if (ImGui::SliderFloat("Transition##Ctrl", &ctrl.DefaultTransitionDuration, 0.0f, 2.0f, "%.2f s"))
                Editor::MarkDirty();

            ImGui::Separator();
            ImGui::Text("Layers");

            // Ensure at least one layer
            if (ctrl.Layers.empty()) {
                ctrl.Layers.resize(1);
                ctrl.Layers[0].ClipIndex = ctrl.CurrentClipIndex;
            }

            for (u32 layerIdx = 0; layerIdx < (u32)ctrl.Layers.size(); layerIdx++) {
                auto& layer = ctrl.Layers[layerIdx];
                std::string layerLabel = (layerIdx == 0) ? "Base Layer" : "Layer " + std::to_string(layerIdx);

                ImGui::PushID((int)layerIdx);
                if (ImGui::TreeNodeEx(layerLabel.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
                    // Clip selector
                    int layerClip = std::clamp(layer.ClipIndex, 0, clipCount - 1);
                    if (ImGui::Combo("Clip##Layer", &layerClip, clipNames.data(), clipCount)) {
                        layer.ClipIndex = layerClip;
                        layer.CurrentTime = 0.0f;
                        Editor::MarkDirty();
                    }

                    // Weight (not shown for base layer — always 1.0)
                    if (layerIdx > 0) {
                        if (ImGui::SliderFloat("Weight##Layer", &layer.Weight, 0.0f, 1.0f, "%.2f"))
                            Editor::MarkDirty();
                    }

                    // Speed
                    if (ImGui::SliderFloat("Speed##Layer", &layer.Speed, 0.0f, 5.0f, "%.2f"))
                        Editor::MarkDirty();

                    // Loop
                    if (ImGui::Checkbox("Loop##Layer", &layer.Loop))
                        Editor::MarkDirty();

                    // Bone mask (only for override layers)
                    if (layerIdx > 0) {
                        const auto& skeleton = model->GetSkeleton();
                        u32 boneCount = skeleton.BoneCount();

                        if (ImGui::TreeNode("Bone Mask##Layer")) {
                            // Resize mask if needed
                            if (layer.BoneMask.size() != boneCount)
                                layer.BoneMask.resize(boneCount, layer.BoneMask.empty());

                            bool allEnabled = true;
                            bool noneEnabled = true;
                            for (u32 b = 0; b < boneCount; b++) {
                                if (layer.BoneMask[b]) noneEnabled = false;
                                else allEnabled = false;
                            }

                            if (ImGui::Button("All")) {
                                std::fill(layer.BoneMask.begin(), layer.BoneMask.end(), true);
                                Editor::MarkDirty();
                            }
                            ImGui::SameLine();
                            if (ImGui::Button("None")) {
                                std::fill(layer.BoneMask.begin(), layer.BoneMask.end(), false);
                                Editor::MarkDirty();
                            }
                            ImGui::SameLine();
                            if (ImGui::Button("Clear Mask")) {
                                layer.BoneMask.clear();
                                Editor::MarkDirty();
                            }

                            for (u32 b = 0; b < boneCount; b++) {
                                bool enabled = layer.BoneMask[b];
                                if (ImGui::Checkbox(skeleton.Bones[b].Name.c_str(), &enabled)) {
                                    layer.BoneMask[b] = enabled;
                                    Editor::MarkDirty();
                                }
                            }
                            ImGui::TreePop();
                        }
                    }

                    // Remove button (not for base layer)
                    if (layerIdx > 0) {
                        if (ImGui::Button("Remove Layer")) {
                            ctrl.Layers.erase(ctrl.Layers.begin() + layerIdx);
                            Editor::MarkDirty();
                            ImGui::TreePop();
                            ImGui::PopID();
                            break;
                        }
                    }

                    ImGui::TreePop();
                }
                ImGui::PopID();
            }

            // Add layer button
            if (ImGui::Button("+ Add Layer##Ctrl")) {
                BlendLayer newLayer;
                newLayer.ClipIndex = 0;
                ctrl.Layers.push_back(newLayer);
                Editor::MarkDirty();
            }
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
                UUID modelUUID;
                if (m_SelectedEntity.HasComponent<MeshRenderer>())
                    modelUUID = m_SelectedEntity.GetComponent<MeshRenderer>().ModelUUID;
                m_SelectedEntity.AddOrReplaceComponent<Animation>(modelUUID);
                ImGui::CloseCurrentPopup();
            }
            if (!m_SelectedEntity.HasComponent<BoneAttachment>() && ImGui::MenuItem("Bone Attachment")) {
                m_SelectedEntity.AddOrReplaceComponent<BoneAttachment>();
                ImGui::CloseCurrentPopup();
            }
            if (m_SelectedEntity.HasComponent<Animation>() &&
                !m_SelectedEntity.HasComponent<AnimationController>() &&
                ImGui::MenuItem("Animation Controller"))
            {
                auto& ctrl = m_SelectedEntity.AddOrReplaceComponent<AnimationController>();
                auto& a = m_SelectedEntity.GetComponent<Animation>();
                BlendLayer baseLayer;
                baseLayer.ClipIndex = a.AnimationIndex;
                baseLayer.Speed = a.Speed;
                baseLayer.Loop = a.Loop;
                ctrl.Layers.push_back(baseLayer);
                ctrl.CurrentClipIndex = a.AnimationIndex;
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
