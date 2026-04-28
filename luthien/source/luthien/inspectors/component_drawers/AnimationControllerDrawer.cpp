#include "lepch.h"
#include "luthien/inspectors/ComponentDrawerRegistry.h"
#include "luthien/inspectors/component_drawers/RegisterComponentDrawers.h"
#include "luthien/widgets/Widgets.h"
#include "luthien/commands/Commands.h"
#include "luthien/CommandHistory.h"
#include "luth/scene/Components.h"
#include "luth/resources/AssetManager.h"
#include "luth/renderer/resources/Model.h"
#include "luth/renderer/resources/AnimationClip.h"

namespace Luth::ComponentDrawers
{
    using namespace Component;

    void RegisterAnimationController()
    {
        ComponentDrawerOptions opts;
        opts.CanAdd = [](Entity e) {
            return e.HasComponent<Animation>() && !e.HasComponent<AnimationController>();
        };
        opts.OnAdd = [](Entity e) {
            auto& a = e.GetComponent<Animation>();

            AnimationController initCtrl;
            BlendLayer baseLayer;
            baseLayer.ClipUUID = a.ClipUUID;  // inherit current clip from Animation
            baseLayer.Speed = a.Speed;
            baseLayer.Loop = (a.LoopMode != AnimationLoopMode::Off);
            initCtrl.Layers.push_back(baseLayer);
            initCtrl.CurrentClipUUID = a.ClipUUID;
            CommandHistory::Execute(std::make_unique<ComponentAddCommand<AnimationController>>(
                "Add AnimationController", e.GetScene(), (entt::entity)e, initCtrl));
        };

        ComponentDrawerRegistry::Register<AnimationController>(
            "Animation Controller",
            [](Entity entity, AnimationController& ctrl) {
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

                const auto& clipUUIDs = model->GetAnimationClipUUIDs();
                if (clipUUIDs.empty()) {
                    ImGui::TextDisabled("No animation clips");
                    return;
                }

                Scene* scene = entity.GetScene();
                entt::entity ent = (entt::entity)entity;

                {
                    UUID oldUUID = ctrl.CurrentClipUUID;
                    UUID picked = ctrl.CurrentClipUUID;
                    if (UI::PropertyAsset("Current Clip##Ctrl", picked, AssetType::Animation)) {
                        // Route through Play() so transitions/crossfade fire correctly.
                        ctrl.Play(picked);
                        EXEC_COMPONENT_PROP("Change Clip", scene, ent, AnimationController, CurrentClipUUID, oldUUID, ctrl.CurrentClipUUID);
                    }
                }

                {
                    auto oldVal = ctrl.ApplyRootMotion;
                    if (ImGui::Checkbox("Root Motion##Ctrl", &ctrl.ApplyRootMotion))
                        EXEC_COMPONENT_PROP("Toggle Root Motion", scene, ent, AnimationController, ApplyRootMotion, oldVal, ctrl.ApplyRootMotion);
                }

                {
                    auto oldVal = ctrl.DefaultTransitionDuration;
                    if (ImGui::SliderFloat("Transition##Ctrl", &ctrl.DefaultTransitionDuration, 0.0f, 2.0f, "%.2f s"))
                        EXEC_COMPONENT_PROP("Change Transition", scene, ent, AnimationController, DefaultTransitionDuration, oldVal, ctrl.DefaultTransitionDuration);
                }

                ImGui::Separator();
                ImGui::Text("Layers");

                if (ctrl.Layers.empty()) {
                    ctrl.Layers.resize(1);
                    ctrl.Layers[0].ClipUUID = ctrl.CurrentClipUUID;
                }

                for (u32 layerIdx = 0; layerIdx < (u32)ctrl.Layers.size(); layerIdx++) {
                    auto& layer = ctrl.Layers[layerIdx];
                    std::string layerLabel = (layerIdx == 0) ? "Base Layer" : "Layer " + std::to_string(layerIdx);

                    ImGui::PushID((int)layerIdx);
                    if (ImGui::TreeNodeEx(layerLabel.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
                        UUID oldClipUUID = layer.ClipUUID;
                        if (UI::PropertyAsset("Clip##Layer", layer.ClipUUID, AssetType::Animation)) {
                            layer.CurrentTime = 0.0f;
                            CommandHistory::Execute(std::make_unique<VectorElementPropertyCommand<AnimationController, BlendLayer, UUID>>(
                                "Change Layer Clip", scene, ent,
                                &AnimationController::Layers, layerIdx, &BlendLayer::ClipUUID,
                                oldClipUUID, layer.ClipUUID));
                        }

                        if (layerIdx > 0) {
                            f32 oldWeight = layer.Weight;
                            if (ImGui::SliderFloat("Weight##Layer", &layer.Weight, 0.0f, 1.0f, "%.2f"))
                                CommandHistory::Execute(std::make_unique<VectorElementPropertyCommand<AnimationController, BlendLayer, f32>>(
                                    "Change Layer Weight", scene, ent,
                                    &AnimationController::Layers, layerIdx, &BlendLayer::Weight,
                                    oldWeight, layer.Weight));
                        }

                        {
                            f32 oldSpeed = layer.Speed;
                            if (ImGui::SliderFloat("Speed##Layer", &layer.Speed, 0.0f, 5.0f, "%.2f"))
                                CommandHistory::Execute(std::make_unique<VectorElementPropertyCommand<AnimationController, BlendLayer, f32>>(
                                    "Change Layer Speed", scene, ent,
                                    &AnimationController::Layers, layerIdx, &BlendLayer::Speed,
                                    oldSpeed, layer.Speed));
                        }

                        {
                            bool oldLoop = layer.Loop;
                            if (ImGui::Checkbox("Loop##Layer", &layer.Loop))
                                CommandHistory::Execute(std::make_unique<VectorElementPropertyCommand<AnimationController, BlendLayer, bool>>(
                                    "Toggle Layer Loop", scene, ent,
                                    &AnimationController::Layers, layerIdx, &BlendLayer::Loop,
                                    oldLoop, layer.Loop));
                        }

                        if (layerIdx > 0) {
                            const auto& skeleton = model->GetSkeleton();
                            u32 boneCount = skeleton.BoneCount();

                            if (ImGui::TreeNode("Bone Mask##Layer")) {
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

                if (ImGui::Button("+ Add Layer##Ctrl")) {
                    BlendLayer newLayer;
                    if (!clipUUIDs.empty()) newLayer.ClipUUID = clipUUIDs[0];
                    CommandHistory::Execute(std::make_unique<VectorInsertCommand<AnimationController, BlendLayer>>(
                        "Add Layer", scene, ent,
                        &AnimationController::Layers, ctrl.Layers.size(), newLayer));
                }
            },
            std::move(opts));
    }
}
