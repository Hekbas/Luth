#include "lepch.h"
#include "luthien/panels/InspectorPanel.h"
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
        LH_CORE_INFO("Created Inspector panel");
    }

    void InspectorPanel::OnInit() {}

    void InspectorPanel::OnRender()
    {
        ImGui::PushFont(Editor::GetFASolid());
        std::string inspector = ICON_FA_CIRCLE_INFO + std::string("  Inspector");

        if (ImGui::Begin(inspector.c_str()))
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

            // Checkbox for active state. Note: Entity::isActive lives on the wrapper;
            // EntityActiveCommand re-resolves the wrapper via UUID on undo/redo.
            bool isActive = m_SelectedEntity.IsActive();
            if (ImGui::Checkbox("##Active", &isActive)) {
                bool oldActive = !isActive;
                m_SelectedEntity.SetActive(isActive);
                CommandHistory::Execute(std::make_unique<EntityActiveCommand>(
                    m_SelectedEntity.GetScene(), (entt::entity)m_SelectedEntity, oldActive, isActive));
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

        // Track the active material for the entity inspector
        UUID activeMaterialUUID = UUID::Invalid();

        // Draw each component with a collapsible UI section
        #if defined(DEBUG)
        DrawComponent<ID>("ID", m_SelectedEntity, [](Entity entity, ID& component) {
            ImGui::Text("ID: %llu", component.Value);
        });

        DrawComponent<Parent>("Parent", m_SelectedEntity, [](Entity entity, Parent& component) {
            if (component.Value && component.Value.IsValid()) {
                ImGui::Text("Parent: %s", component.Value.GetName().c_str());
                if (ImGui::Button("Clear Parent")) {
                    entity.SetParent({});
                }
            }
            else {
                ImGui::Text("No Parent");
            }
        });

        DrawComponent<Children>("Children", m_SelectedEntity, [](Entity entity, Children& component) {
            ImGui::Text("Children: %d", component.Value.size());
            for (auto& child : component.Value) {
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
                {
                    auto old = transform.Position;
                    if (UI::Property("Position", transform.Position)) {
                        CommandHistory::Execute(std::make_unique<ComponentPropertyCommand<Transform, Vec3>>(
                            "Change Position", entity.GetScene(), (entt::entity)entity,
                            &Transform::Position, old, transform.Position));
                        transform.IsDirty = true;
                    }
                }
                {
                    auto old = transform.Rotation;
                    if (UI::Property("Rotation", transform.Rotation)) {
                        CommandHistory::Execute(std::make_unique<ComponentPropertyCommand<Transform, Vec3>>(
                            "Change Rotation", entity.GetScene(), (entt::entity)entity,
                            &Transform::Rotation, old, transform.Rotation));
                        transform.IsDirty = true;
                    }
                }
                {
                    auto old = transform.Scale;
                    if (UI::Property("Scale", transform.Scale, 0.1f, 1.0f)) {
                        CommandHistory::Execute(std::make_unique<ComponentPropertyCommand<Transform, Vec3>>(
                            "Change Scale", entity.GetScene(), (entt::entity)entity,
                            &Transform::Scale, old, transform.Scale));
                        transform.IsDirty = true;
                    }
                }
                UI::EndProperties();
            }
        });

        DrawComponent<Camera>("Camera", m_SelectedEntity, [](Entity e, Camera& camera) {
            if (UI::BeginProperties("CameraProps")) {
                Scene* scene = e.GetScene();
                entt::entity ent = (entt::entity)e;

                const char* projectionTypeStrings[] = { "Perspective", "Orthographic" };
                int currentProj = (int)camera.Projection;
                if (UI::PropertyCombo("Projection", currentProj, projectionTypeStrings, 2)) {
                    auto oldProj = camera.Projection;
                    camera.Projection = (Camera::ProjectionType)currentProj;
                    CommandHistory::Execute(std::make_unique<ComponentPropertyCommand<Camera, Camera::ProjectionType>>(
                        "Change Projection", scene, ent,
                        &Camera::Projection, oldProj, camera.Projection));
                    camera.IsDirty = true;
                }

                // Capture all values before UI edits
                auto snapFOV = camera.VerticalFOV;
                auto snapNear = camera.NearClip;
                auto snapFar = camera.FarClip;
                auto snapOrthoSize = camera.OrthographicSize;
                auto snapOrthoNear = camera.OrthographicNear;
                auto snapOrthoFar = camera.OrthographicFar;
                auto snapAspect = camera.AspectRatio;

                bool changed = false;
                if (camera.Projection == Camera::ProjectionType::Perspective) {
                    if (UI::Property("FOV", camera.VerticalFOV, 0.1f, 1.0f, 180.0f)) {
                        CommandHistory::Execute(std::make_unique<ComponentPropertyCommand<Camera, float>>(
                            "Change FOV", scene, ent, &Camera::VerticalFOV, snapFOV, camera.VerticalFOV));
                        changed = true;
                    }
                    if (UI::Property("Near", camera.NearClip, 0.01f, 0.01f, camera.FarClip)) {
                        CommandHistory::Execute(std::make_unique<ComponentPropertyCommand<Camera, float>>(
                            "Change Near Clip", scene, ent, &Camera::NearClip, snapNear, camera.NearClip));
                        changed = true;
                    }
                    if (UI::Property("Far", camera.FarClip, 0.1f, camera.NearClip, 10000.0f)) {
                        CommandHistory::Execute(std::make_unique<ComponentPropertyCommand<Camera, float>>(
                            "Change Far Clip", scene, ent, &Camera::FarClip, snapFar, camera.FarClip));
                        changed = true;
                    }
                }
                else {
                    if (UI::Property("Size", camera.OrthographicSize, 0.1f, 0.1f, 100.0f)) {
                        CommandHistory::Execute(std::make_unique<ComponentPropertyCommand<Camera, float>>(
                            "Change Ortho Size", scene, ent, &Camera::OrthographicSize, snapOrthoSize, camera.OrthographicSize));
                        changed = true;
                    }
                    if (UI::Property("Near", camera.OrthographicNear, 0.01f)) {
                        CommandHistory::Execute(std::make_unique<ComponentPropertyCommand<Camera, float>>(
                            "Change Ortho Near", scene, ent, &Camera::OrthographicNear, snapOrthoNear, camera.OrthographicNear));
                        changed = true;
                    }
                    if (UI::Property("Far", camera.OrthographicFar, 0.01f)) {
                        CommandHistory::Execute(std::make_unique<ComponentPropertyCommand<Camera, float>>(
                            "Change Ortho Far", scene, ent, &Camera::OrthographicFar, snapOrthoFar, camera.OrthographicFar));
                        changed = true;
                    }
                }

                if (UI::Property("Aspect", camera.AspectRatio, 0.01f, 0.1f, 10.0f)) {
                    CommandHistory::Execute(std::make_unique<ComponentPropertyCommand<Camera, float>>(
                        "Change Aspect", scene, ent, &Camera::AspectRatio, snapAspect, camera.AspectRatio));
                    changed = true;
                }

                if (changed) camera.IsDirty = true;
                UI::EndProperties();
            }
        });

        DrawComponent<MeshRenderer>("Mesh Renderer", m_SelectedEntity, [&](Entity entity, MeshRenderer& meshRenderer) {
            if (UI::BeginProperties()) {
                Scene* scene = entity.GetScene();
                entt::entity ent = (entt::entity)entity;

                {
                    auto oldUUID = meshRenderer.ModelUUID;
                    if (UI::PropertyAsset("Model", meshRenderer.ModelUUID, AssetType::Model)) {
                        CommandHistory::Execute(std::make_unique<ComponentPropertyCommand<MeshRenderer, UUID>>(
                            "Change Model", scene, ent, &MeshRenderer::ModelUUID, oldUUID, meshRenderer.ModelUUID));
                    }
                }

                // Ensure model is loaded to get mesh count
                if (meshRenderer.ModelUUID.IsValid() && !AssetManager::IsLoaded(meshRenderer.ModelUUID) && !AssetManager::IsLoading(meshRenderer.ModelUUID))
                     AssetManager::LoadAsync(meshRenderer.ModelUUID);

                if (auto model = AssetManager::GetAsset<Model>(meshRenderer.ModelUUID)) {
                    int meshIndex = (int)meshRenderer.MeshIndex;
                    auto oldIndex = meshRenderer.MeshIndex;
                    if (UI::Property("Mesh Index", meshIndex, 0, (int)model->GetMeshes().size() - 1)) {
                        meshRenderer.MeshIndex = (u32)meshIndex;
                        CommandHistory::Execute(std::make_unique<ComponentPropertyCommand<MeshRenderer, uint32_t>>(
                            "Change Mesh Index", scene, ent, &MeshRenderer::MeshIndex, oldIndex, meshRenderer.MeshIndex));
                    }
                }

                {
                    auto oldMatUUID = meshRenderer.MaterialUUID;
                    if (UI::PropertyAsset("Material", meshRenderer.MaterialUUID, AssetType::Material))
                    {
                        if (auto model = AssetManager::GetAsset<Model>(meshRenderer.ModelUUID))
                            model->AddMaterial(meshRenderer.MaterialUUID, meshRenderer.MeshIndex);
                        CommandHistory::Execute(std::make_unique<ComponentPropertyCommand<MeshRenderer, UUID>>(
                            "Change Material", scene, ent, &MeshRenderer::MaterialUUID, oldMatUUID, meshRenderer.MaterialUUID));
                    }
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
            
            if (UI::BeginProperties("AnimProps")) {
                Scene* scene = entity.GetScene();
                entt::entity ent = (entt::entity)entity;

                {
                    auto oldClip = animation.AnimationIndex;
                    if (UI::PropertyCombo("Clip", animation.AnimationIndex, clipNames.data(), clipCount)) {
                        animation.CurrentTime = 0.0f;
                        CommandHistory::Execute(std::make_unique<ComponentPropertyCommand<Animation, i32>>(
                            "Change Clip", scene, ent, &Animation::AnimationIndex, oldClip, animation.AnimationIndex));
                    }
                }

                const AnimationClip* clip = model->GetAnimationClip((u32)animation.AnimationIndex);
                if (clip) {
                    f32 duration = clip->GetDurationSeconds();

                    auto oldSpeed = animation.Speed;
                    if (UI::Property("Speed", animation.Speed, 0.05f, 0.0f, 5.0f))
                        CommandHistory::Execute(std::make_unique<ComponentPropertyCommand<Animation, f32>>(
                            "Change Speed", scene, ent, &Animation::Speed, oldSpeed, animation.Speed));

                    if (duration > 0.0f) {
                        auto oldTime = animation.CurrentTime;
                        if (UI::Property("Timeline", animation.CurrentTime, 0.01f, 0.0f, duration))
                            CommandHistory::Execute(std::make_unique<ComponentPropertyCommand<Animation, f32>>(
                                "Change Timeline", scene, ent, &Animation::CurrentTime, oldTime, animation.CurrentTime));
                    }
                }

                // Editor preference, not scene state — no command + no scene MarkDirty.
                UI::Property("Show Bones", Editor::GetSettings().showBoneDebug);

                UI::EndProperties();
            }

            const AnimationClip* clip = model->GetAnimationClip((u32)animation.AnimationIndex);
            if (!clip) return;

            // Transport controls
            ImGui::Dummy({ 0, 4 });
            
            float buttonWidth = 32.0f;
            float spacing = 4.0f;
            float totalWidth = (buttonWidth * 3) + (spacing * 2);
            AlignItemToCenter(totalWidth);

            ImGui::BeginGroup();
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(spacing, 4.0f));

            if (animation.Playing) {
                if (ImGui::Button(ICON_FA_PAUSE "##AnimPause", ImVec2(buttonWidth, 24))) {
                    CommandHistory::Execute(std::make_unique<ComponentPropertyCommand<Animation, bool>>(
                        "Pause Animation", entity.GetScene(), (entt::entity)entity,
                        &Animation::Playing, true, false));
                }
            } else {
                if (ImGui::Button(ICON_FA_PLAY "##AnimPlay", ImVec2(buttonWidth, 24))) {
                    CommandHistory::Execute(std::make_unique<ComponentPropertyCommand<Animation, bool>>(
                        "Play Animation", entity.GetScene(), (entt::entity)entity,
                        &Animation::Playing, false, true));
                }
            }
            ImGui::SameLine();

            if (ImGui::Button(ICON_FA_STOP "##AnimStop", ImVec2(buttonWidth, 24))) {
                CommandHistory::BeginCompound("Stop Animation");
                CommandHistory::Execute(std::make_unique<ComponentPropertyCommand<Animation, bool>>(
                    "Stop Playing", entity.GetScene(), (entt::entity)entity,
                    &Animation::Playing, animation.Playing, false));
                CommandHistory::Execute(std::make_unique<ComponentPropertyCommand<Animation, f32>>(
                    "Reset Time", entity.GetScene(), (entt::entity)entity,
                    &Animation::CurrentTime, animation.CurrentTime, 0.0f));
                CommandHistory::EndCompound();
            }
            ImGui::SameLine();

            // Loop off/one/all toggle
            const char* loopIcon = ICON_FA_ARROW_RIGHT; // Off
            const char* loopTooltip = "Loop: Off";
            if (animation.LoopMode == AnimationLoopMode::One) {
                loopIcon = ICON_FA_ARROW_ROTATE_RIGHT;
                loopTooltip = "Loop: One";
            }
            else if (animation.LoopMode == AnimationLoopMode::All) {
                loopIcon = ICON_FA_ARROWS_ROTATE;
                loopTooltip = "Loop: All";
            }

            if (ImGui::Button(loopIcon, ImVec2(buttonWidth, 24))) {
                auto oldMode = animation.LoopMode;
                int next = ((int)animation.LoopMode + 1) % 3;
                animation.LoopMode = (AnimationLoopMode)next;
                CommandHistory::Execute(std::make_unique<ComponentPropertyCommand<Animation, AnimationLoopMode>>(
                    "Change Loop Mode", entity.GetScene(), (entt::entity)entity,
                    &Animation::LoopMode, oldMode, animation.LoopMode));
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", loopTooltip);

            ImGui::PopStyleVar(2);
            ImGui::EndGroup();

            ImGui::Dummy({ 0, 4 });
            
            f32 tps = (clip->TicksPerSecond > 0.0f) ? clip->TicksPerSecond : 25.0f;
            int frame = (int)(animation.CurrentTime * tps);
            int totalFrames = (int)clip->Duration;
            
            AlignItemToCenter(100);
            ImGui::TextDisabled("Frame: %d / %d", frame, totalFrames);
            ImGui::Dummy({ 0, 4 });
        });

        DrawComponent<BoneAttachment>("Bone Attachment", m_SelectedEntity, [](Entity entity, BoneAttachment& attachment) {
            Scene* scene = entity.GetScene();
            if (!scene) return;

            // Target entity combo — list all entities with Animation + Tag
            auto& registry = scene->Registry();
            std::vector<entt::entity> animEntities;
            std::vector<std::string> entityNames;
            
            // "None" option first
            entityNames.push_back("None");

            int currentIndex = 0;
            auto view = registry.view<Animation, Tag>();
            for (auto e : view) {
                animEntities.push_back(e);
                entityNames.push_back(registry.get<Tag>(e).Value);
                if (attachment.TargetEntity && (entt::entity)attachment.TargetEntity == e)
                    currentIndex = (int)animEntities.size(); // 1-based index relates to entityNames
            }

            std::vector<const char*> entityNamePtrs(entityNames.size());
            for(size_t i = 0; i < entityNames.size(); i++)
                entityNamePtrs[i] = entityNames[i].c_str();

            if (UI::BeginProperties("BoneAttachProps")) {
                if (UI::PropertyCombo("Target", currentIndex, entityNamePtrs.data(), (int)entityNamePtrs.size())) {
                    Entity      oldTarget = attachment.TargetEntity;
                    i32         oldIndex  = attachment.BoneIndex;
                    std::string oldName   = attachment.BoneName;

                    if (currentIndex == 0) {
                        attachment.TargetEntity = {};
                        attachment.BoneIndex = -1;
                        attachment.BoneName = "";
                    } else {
                        attachment.TargetEntity = Entity(animEntities[currentIndex - 1], scene);
                        attachment.BoneIndex = -1;
                        attachment.BoneName = "";
                    }

                    CommandHistory::BeginCompound("Set Bone Attachment Target");
                    CommandHistory::Execute(std::make_unique<ComponentPropertyCommand<BoneAttachment, Entity>>(
                        "Target", scene, (entt::entity)entity,
                        &BoneAttachment::TargetEntity, oldTarget, attachment.TargetEntity));
                    CommandHistory::Execute(std::make_unique<ComponentPropertyCommand<BoneAttachment, i32>>(
                        "BoneIndex", scene, (entt::entity)entity,
                        &BoneAttachment::BoneIndex, oldIndex, attachment.BoneIndex));
                    CommandHistory::Execute(std::make_unique<ComponentPropertyCommand<BoneAttachment, std::string>>(
                        "BoneName", scene, (entt::entity)entity,
                        &BoneAttachment::BoneName, oldName, attachment.BoneName));
                    CommandHistory::EndCompound();
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

                            if (UI::PropertyCombo("Bone", boneIdx, boneNames.data(), boneCount)) {
                                std::string oldName  = attachment.BoneName;
                                i32         oldIndex = attachment.BoneIndex;
                                attachment.BoneName = skeleton.Bones[boneIdx].Name;
                                attachment.BoneIndex = -1; // Force re-resolve by AnimationSystem

                                CommandHistory::BeginCompound("Set Attached Bone");
                                CommandHistory::Execute(std::make_unique<ComponentPropertyCommand<BoneAttachment, std::string>>(
                                    "BoneName", scene, (entt::entity)entity,
                                    &BoneAttachment::BoneName, oldName, attachment.BoneName));
                                CommandHistory::Execute(std::make_unique<ComponentPropertyCommand<BoneAttachment, i32>>(
                                    "BoneIndex", scene, (entt::entity)entity,
                                    &BoneAttachment::BoneIndex, oldIndex, attachment.BoneIndex));
                                CommandHistory::EndCompound();
                            }
                        }
                    }
                }

                {
                    auto oldOffset = attachment.LocalOffset;
                    if (UI::Property("Local Offset", attachment.LocalOffset))
                        CommandHistory::Execute(std::make_unique<ComponentPropertyCommand<BoneAttachment, Vec3>>(
                            "Change Local Offset", entity.GetScene(), (entt::entity)entity,
                            &BoneAttachment::LocalOffset, oldOffset, attachment.LocalOffset));
                }
                {
                    auto oldRot = attachment.LocalRotation;
                    if (UI::Property("Local Rotation", attachment.LocalRotation))
                        CommandHistory::Execute(std::make_unique<ComponentPropertyCommand<BoneAttachment, Vec3>>(
                            "Change Local Rotation", entity.GetScene(), (entt::entity)entity,
                            &BoneAttachment::LocalRotation, oldRot, attachment.LocalRotation));
                }
                    
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

            Scene* scene = entity.GetScene();
            entt::entity ent = (entt::entity)entity;

            // Current clip selector (base layer)
            int currentClip = ctrl.CurrentClipIndex;
            currentClip = std::clamp(currentClip, 0, clipCount - 1);
            if (ImGui::Combo("Current Clip##Ctrl", &currentClip, clipNames.data(), clipCount)) {
                auto oldClip = ctrl.CurrentClipIndex;
                ctrl.Play(currentClip);
                CommandHistory::Execute(std::make_unique<ComponentPropertyCommand<AnimationController, int>>(
                    "Change Clip", scene, ent, &AnimationController::CurrentClipIndex, oldClip, ctrl.CurrentClipIndex));
            }

            // Root motion
            {
                auto oldVal = ctrl.ApplyRootMotion;
                if (ImGui::Checkbox("Root Motion##Ctrl", &ctrl.ApplyRootMotion))
                    CommandHistory::Execute(std::make_unique<ComponentPropertyCommand<AnimationController, bool>>(
                        "Toggle Root Motion", scene, ent, &AnimationController::ApplyRootMotion, oldVal, ctrl.ApplyRootMotion));
            }

            // Default transition duration
            {
                auto oldVal = ctrl.DefaultTransitionDuration;
                if (ImGui::SliderFloat("Transition##Ctrl", &ctrl.DefaultTransitionDuration, 0.0f, 2.0f, "%.2f s"))
                    CommandHistory::Execute(std::make_unique<ComponentPropertyCommand<AnimationController, float>>(
                        "Change Transition", scene, ent, &AnimationController::DefaultTransitionDuration, oldVal, ctrl.DefaultTransitionDuration));
            }

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
                    // Clip selector. CurrentTime reset on change is a runtime side effect
                    // (playback position regenerates each frame) — only ClipIndex is undoable.
                    int layerClip = std::clamp(layer.ClipIndex, 0, clipCount - 1);
                    int oldClipIdx = layer.ClipIndex;
                    if (ImGui::Combo("Clip##Layer", &layerClip, clipNames.data(), clipCount)) {
                        layer.ClipIndex = layerClip;
                        layer.CurrentTime = 0.0f;
                        CommandHistory::Execute(std::make_unique<VectorElementPropertyCommand<AnimationController, BlendLayer, i32>>(
                            "Change Layer Clip", scene, ent,
                            &AnimationController::Layers, layerIdx, &BlendLayer::ClipIndex,
                            oldClipIdx, layerClip));
                    }

                    // Weight (not shown for base layer — always 1.0)
                    if (layerIdx > 0) {
                        f32 oldWeight = layer.Weight;
                        if (ImGui::SliderFloat("Weight##Layer", &layer.Weight, 0.0f, 1.0f, "%.2f"))
                            CommandHistory::Execute(std::make_unique<VectorElementPropertyCommand<AnimationController, BlendLayer, f32>>(
                                "Change Layer Weight", scene, ent,
                                &AnimationController::Layers, layerIdx, &BlendLayer::Weight,
                                oldWeight, layer.Weight));
                    }

                    // Speed
                    {
                        f32 oldSpeed = layer.Speed;
                        if (ImGui::SliderFloat("Speed##Layer", &layer.Speed, 0.0f, 5.0f, "%.2f"))
                            CommandHistory::Execute(std::make_unique<VectorElementPropertyCommand<AnimationController, BlendLayer, f32>>(
                                "Change Layer Speed", scene, ent,
                                &AnimationController::Layers, layerIdx, &BlendLayer::Speed,
                                oldSpeed, layer.Speed));
                    }

                    // Loop
                    {
                        bool oldLoop = layer.Loop;
                        if (ImGui::Checkbox("Loop##Layer", &layer.Loop))
                            CommandHistory::Execute(std::make_unique<VectorElementPropertyCommand<AnimationController, BlendLayer, bool>>(
                                "Toggle Layer Loop", scene, ent,
                                &AnimationController::Layers, layerIdx, &BlendLayer::Loop,
                                oldLoop, layer.Loop));
                    }

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
                                auto oldMask = layer.BoneMask;
                                std::fill(layer.BoneMask.begin(), layer.BoneMask.end(), true);
                                CommandHistory::Execute(std::make_unique<VectorElementPropertyCommand<AnimationController, BlendLayer, std::vector<bool>>>(
                                    "Set Bone Mask All", scene, ent,
                                    &AnimationController::Layers, layerIdx, &BlendLayer::BoneMask,
                                    oldMask, layer.BoneMask));
                            }
                            ImGui::SameLine();
                            if (ImGui::Button("None")) {
                                auto oldMask = layer.BoneMask;
                                std::fill(layer.BoneMask.begin(), layer.BoneMask.end(), false);
                                CommandHistory::Execute(std::make_unique<VectorElementPropertyCommand<AnimationController, BlendLayer, std::vector<bool>>>(
                                    "Set Bone Mask None", scene, ent,
                                    &AnimationController::Layers, layerIdx, &BlendLayer::BoneMask,
                                    oldMask, layer.BoneMask));
                            }
                            ImGui::SameLine();
                            if (ImGui::Button("Clear Mask")) {
                                auto oldMask = layer.BoneMask;
                                layer.BoneMask.clear();
                                CommandHistory::Execute(std::make_unique<VectorElementPropertyCommand<AnimationController, BlendLayer, std::vector<bool>>>(
                                    "Clear Bone Mask", scene, ent,
                                    &AnimationController::Layers, layerIdx, &BlendLayer::BoneMask,
                                    oldMask, layer.BoneMask));
                            }

                            for (u32 b = 0; b < boneCount; b++) {
                                bool enabled = layer.BoneMask[b];
                                if (ImGui::Checkbox(skeleton.Bones[b].Name.c_str(), &enabled)) {
                                    auto oldMask = layer.BoneMask;
                                    layer.BoneMask[b] = enabled;
                                    CommandHistory::Execute(std::make_unique<VectorElementPropertyCommand<AnimationController, BlendLayer, std::vector<bool>>>(
                                        "Toggle Bone Mask Bit", scene, ent,
                                        &AnimationController::Layers, layerIdx, &BlendLayer::BoneMask,
                                        oldMask, layer.BoneMask));
                                }
                            }
                            ImGui::TreePop();
                        }
                    }

                    // Remove button (not for base layer)
                    if (layerIdx > 0) {
                        if (ImGui::Button("Remove Layer")) {
                            CommandHistory::Execute(std::make_unique<VectorEraseCommand<AnimationController, BlendLayer>>(
                                "Remove Layer", scene, ent,
                                &AnimationController::Layers, layerIdx));
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
                CommandHistory::Execute(std::make_unique<VectorInsertCommand<AnimationController, BlendLayer>>(
                    "Add Layer", scene, ent,
                    &AnimationController::Layers, ctrl.Layers.size(), newLayer));
            }
        });

        DrawComponent<DirectionalLight>("Directional Light", m_SelectedEntity, [](Entity entity, DirectionalLight& dirLight) {
            if (UI::BeginProperties()) {
                Scene* scene = entity.GetScene();
                entt::entity ent = (entt::entity)entity;

                auto oldColor = dirLight.Color;
                if (UI::PropertyColor("Color", dirLight.Color))
                    CommandHistory::Execute(std::make_unique<ComponentPropertyCommand<DirectionalLight, Vec3>>(
                        "Change Light Color", scene, ent, &DirectionalLight::Color, oldColor, dirLight.Color));

                auto oldIntensity = dirLight.Intensity;
                if (UI::Property("Intensity", dirLight.Intensity, 0.1f, 0.0f, 1000.0f))
                    CommandHistory::Execute(std::make_unique<ComponentPropertyCommand<DirectionalLight, float>>(
                        "Change Light Intensity", scene, ent, &DirectionalLight::Intensity, oldIntensity, dirLight.Intensity));

                auto oldCastShadows = dirLight.CastShadows;
                if (UI::Property("Cast Shadows", dirLight.CastShadows))
                    CommandHistory::Execute(std::make_unique<ComponentPropertyCommand<DirectionalLight, bool>>(
                        "Toggle Cast Shadows", scene, ent, &DirectionalLight::CastShadows, oldCastShadows, dirLight.CastShadows));

                if (dirLight.CastShadows) {
                    // Per-cascade CSM bias UI; show cascade-0 bias here for now (no undo).
                    UI::Property("Shadow Bias (C0)", dirLight.ShadowBias[0], 0.0001f, 0.0f, 0.05f);
                    UI::Property("Normal Bias (texels)", dirLight.ShadowNormalBias[0], 0.1f, 0.0f, 10.0f);
                    UI::Property("Blend Width", dirLight.CascadeBlendWidth, 0.01f, 0.0f, 1.0f);
                    UI::Property("Show Cascades", dirLight.DebugVisualizeCascades);

                    auto oldOrtho = dirLight.ShadowOrthoSize;
                    if (UI::Property("Shadow Size", dirLight.ShadowOrthoSize, 1.0f, 10.0f, 2000.0f))
                        CommandHistory::Execute(std::make_unique<ComponentPropertyCommand<DirectionalLight, float>>(
                            "Change Shadow Size", scene, ent, &DirectionalLight::ShadowOrthoSize, oldOrtho, dirLight.ShadowOrthoSize));

                    auto oldDist = dirLight.ShadowDistance;
                    if (UI::Property("Shadow Distance", dirLight.ShadowDistance, 1.0f, 10.0f, 2000.0f))
                        CommandHistory::Execute(std::make_unique<ComponentPropertyCommand<DirectionalLight, float>>(
                            "Change Shadow Distance", scene, ent, &DirectionalLight::ShadowDistance, oldDist, dirLight.ShadowDistance));
                }
                UI::EndProperties();
            }
        });

        DrawComponent<PointLight>("Point Light", m_SelectedEntity, [](Entity entity, PointLight& pointLight) {
            if (UI::BeginProperties()) {
                Scene* scene = entity.GetScene();
                entt::entity ent = (entt::entity)entity;

                auto oldColor = pointLight.Color;
                if (UI::PropertyColor("Color", pointLight.Color))
                    CommandHistory::Execute(std::make_unique<ComponentPropertyCommand<PointLight, Vec3>>(
                        "Change Light Color", scene, ent, &PointLight::Color, oldColor, pointLight.Color));

                auto oldIntensity = pointLight.Intensity;
                if (UI::Property("Intensity", pointLight.Intensity, 0.1f, 0.0f, 1000.0f))
                    CommandHistory::Execute(std::make_unique<ComponentPropertyCommand<PointLight, float>>(
                        "Change Light Intensity", scene, ent, &PointLight::Intensity, oldIntensity, pointLight.Intensity));

                auto oldRange = pointLight.Range;
                if (UI::Property("Range", pointLight.Range, 0.1f, 0.0f, 10000.0f))
                    CommandHistory::Execute(std::make_unique<ComponentPropertyCommand<PointLight, float>>(
                        "Change Light Range", scene, ent, &PointLight::Range, oldRange, pointLight.Range));

                UI::EndProperties();
            }
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
                CommandHistory::Execute(std::make_unique<ComponentAddCommand<Camera>>(
                    "Add Camera", m_SelectedEntity.GetScene(), (entt::entity)m_SelectedEntity));
                ImGui::CloseCurrentPopup();
            }
            if (!m_SelectedEntity.HasComponent<DirectionalLight>() && ImGui::MenuItem("Directional Light")) {
                CommandHistory::Execute(std::make_unique<ComponentAddCommand<DirectionalLight>>(
                    "Add DirectionalLight", m_SelectedEntity.GetScene(), (entt::entity)m_SelectedEntity));
                ImGui::CloseCurrentPopup();
            }
            if (!m_SelectedEntity.HasComponent<PointLight>() && ImGui::MenuItem("Point Light")) {
                CommandHistory::Execute(std::make_unique<ComponentAddCommand<PointLight>>(
                    "Add PointLight", m_SelectedEntity.GetScene(), (entt::entity)m_SelectedEntity));
                ImGui::CloseCurrentPopup();
            }
            if (!m_SelectedEntity.HasComponent<MeshRenderer>() && ImGui::MenuItem("Mesh Renderer")) {
                CommandHistory::Execute(std::make_unique<ComponentAddCommand<MeshRenderer>>(
                    "Add MeshRenderer", m_SelectedEntity.GetScene(), (entt::entity)m_SelectedEntity));
                ImGui::CloseCurrentPopup();
            }
            if (!m_SelectedEntity.HasComponent<Animation>() && ImGui::MenuItem("Animation")) {
                UUID modelUUID;
                if (m_SelectedEntity.HasComponent<MeshRenderer>())
                    modelUUID = m_SelectedEntity.GetComponent<MeshRenderer>().ModelUUID;
                Animation initAnim(modelUUID);
                CommandHistory::Execute(std::make_unique<ComponentAddCommand<Animation>>(
                    "Add Animation", m_SelectedEntity.GetScene(), (entt::entity)m_SelectedEntity, initAnim));
                ImGui::CloseCurrentPopup();
            }
            if (!m_SelectedEntity.HasComponent<BoneAttachment>() && ImGui::MenuItem("Bone Attachment")) {
                CommandHistory::Execute(std::make_unique<ComponentAddCommand<BoneAttachment>>(
                    "Add BoneAttachment", m_SelectedEntity.GetScene(), (entt::entity)m_SelectedEntity));
                ImGui::CloseCurrentPopup();
            }
            if (m_SelectedEntity.HasComponent<Animation>() &&
                !m_SelectedEntity.HasComponent<AnimationController>() &&
                ImGui::MenuItem("Animation Controller"))
            {
                auto& a = m_SelectedEntity.GetComponent<Animation>();
                AnimationController initCtrl;
                BlendLayer baseLayer;
                baseLayer.ClipIndex = a.AnimationIndex;
                baseLayer.Speed = a.Speed;
                baseLayer.Loop = (a.LoopMode != AnimationLoopMode::Off);
                initCtrl.Layers.push_back(baseLayer);
                initCtrl.CurrentClipIndex = a.AnimationIndex;
                CommandHistory::Execute(std::make_unique<ComponentAddCommand<AnimationController>>(
                    "Add AnimationController", m_SelectedEntity.GetScene(), (entt::entity)m_SelectedEntity, initCtrl));
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
                    CommandHistory::Execute(std::make_unique<ComponentRemoveCommand<T>>(
                        "Remove Component", entity.GetScene(), (entt::entity)entity));
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
