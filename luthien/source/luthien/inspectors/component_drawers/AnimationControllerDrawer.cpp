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

#include <nlohmann/json.hpp>

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
        opts.OnCopy = [](Entity e) {
            const auto& ctrl = e.GetComponent<AnimationController>();
            nlohmann::json j;
            j["currentClipUUID"]           = ctrl.CurrentClipUUID.ToString();
            j["applyRootMotion"]           = ctrl.ApplyRootMotion;
            j["defaultTransitionDuration"] = ctrl.DefaultTransitionDuration;
            nlohmann::json layers = nlohmann::json::array();
            for (const auto& layer : ctrl.Layers) {
                nlohmann::json lj;
                lj["clipUUID"] = layer.ClipUUID.ToString();
                lj["speed"]    = layer.Speed;
                lj["weight"]   = layer.Weight;
                lj["loop"]     = layer.Loop;
                layers.push_back(lj);
            }
            j["layers"] = layers;
            return j.dump();
        };
        // BoneMask intentionally omitted — bone indices are skeleton-specific.
        opts.OnPaste = [](Entity e, const std::string& data) -> bool {
            try {
                auto j = nlohmann::json::parse(data);
                AnimationController newCtrl = e.GetComponent<AnimationController>();
                newCtrl.CurrentClipUUID           = UUID::FromString(j.value("currentClipUUID", ""));
                newCtrl.ApplyRootMotion           = j.value("applyRootMotion", false);
                newCtrl.DefaultTransitionDuration = j.value("defaultTransitionDuration", 0.2f);
                newCtrl.Layers.clear();
                if (j.contains("layers")) {
                    for (const auto& lj : j["layers"]) {
                        BlendLayer layer;
                        layer.ClipUUID = UUID::FromString(lj.value("clipUUID", ""));
                        layer.Speed    = lj.value("speed", 1.0f);
                        layer.Weight   = lj.value("weight", 1.0f);
                        layer.Loop     = lj.value("loop", true);
                        newCtrl.Layers.push_back(std::move(layer));
                    }
                }
                newCtrl.ActiveTransition.reset();
                CommandHistory::Execute(std::make_unique<ComponentReplaceCommand<AnimationController>>(
                    "Paste AnimationController", e.GetScene(), (entt::entity)e, std::move(newCtrl)));
                return true;
            } catch (...) { return false; }
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
                    UUID picked = ctrl.CurrentClipUUID;
                    auto state = UI::PropertyAsset("Current Clip##Ctrl", picked, AssetType::Animation);
                    if (state.committed) {
                        UUID oldUUID = UI::ConsumeItemPreEdit<UUID>(state.itemId);
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
                    f32 pre = ctrl.DefaultTransitionDuration;
                    ImGui::SliderFloat("Transition##Ctrl", &ctrl.DefaultTransitionDuration, 0.0f, 2.0f, "%.2f s");
                    ImGuiID id = ImGui::GetItemID();
                    if (ImGui::IsItemActivated()) UI::SaveItemPreEdit<f32>(id, pre);
                    if (ImGui::IsItemDeactivatedAfterEdit())
                        EXEC_COMPONENT_PROP("Change Transition", scene, ent, AnimationController, DefaultTransitionDuration,
                                            UI::ConsumeItemPreEdit<f32>(id), ctrl.DefaultTransitionDuration);
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
                        {
                            auto state = UI::PropertyAsset("Clip##Layer", layer.ClipUUID, AssetType::Animation);
                            if (state.committed) {
                                layer.CurrentTime = 0.0f;
                                CommandHistory::Execute(std::make_unique<VectorElementPropertyCommand<AnimationController, BlendLayer, UUID>>(
                                    "Change Layer Clip", scene, ent,
                                    &AnimationController::Layers, layerIdx, &BlendLayer::ClipUUID,
                                    UI::ConsumeItemPreEdit<UUID>(state.itemId), layer.ClipUUID));
                            }
                        }

                        if (layerIdx > 0) {
                            f32 pre = layer.Weight;
                            ImGui::SliderFloat("Weight##Layer", &layer.Weight, 0.0f, 1.0f, "%.2f");
                            ImGuiID id = ImGui::GetItemID();
                            if (ImGui::IsItemActivated()) UI::SaveItemPreEdit<f32>(id, pre);
                            if (ImGui::IsItemDeactivatedAfterEdit())
                                CommandHistory::Execute(std::make_unique<VectorElementPropertyCommand<AnimationController, BlendLayer, f32>>(
                                    "Change Layer Weight", scene, ent,
                                    &AnimationController::Layers, layerIdx, &BlendLayer::Weight,
                                    UI::ConsumeItemPreEdit<f32>(id), layer.Weight));
                        }

                        {
                            f32 pre = layer.Speed;
                            ImGui::SliderFloat("Speed##Layer", &layer.Speed, 0.0f, 5.0f, "%.2f");
                            ImGuiID id = ImGui::GetItemID();
                            if (ImGui::IsItemActivated()) UI::SaveItemPreEdit<f32>(id, pre);
                            if (ImGui::IsItemDeactivatedAfterEdit())
                                CommandHistory::Execute(std::make_unique<VectorElementPropertyCommand<AnimationController, BlendLayer, f32>>(
                                    "Change Layer Speed", scene, ent,
                                    &AnimationController::Layers, layerIdx, &BlendLayer::Speed,
                                    UI::ConsumeItemPreEdit<f32>(id), layer.Speed));
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
