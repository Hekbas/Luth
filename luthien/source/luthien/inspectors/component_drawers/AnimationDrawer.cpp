#include "lepch.h"
#include "luthien/inspectors/ComponentDrawerRegistry.h"
#include "luthien/inspectors/component_drawers/RegisterComponentDrawers.h"
#include "luthien/Editor.h"
#include "luthien/widgets/Widgets.h"
#include "luthien/widgets/ImGuiUtils.h"
#include "luthien/widgets/Icons.h"
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

    void RegisterAnimation()
    {
        ComponentDrawerOptions opts;
        opts.OnAdd = [](Entity e) {
            UUID modelUUID;
            if (e.HasComponent<MeshRenderer>())
                modelUUID = e.GetComponent<MeshRenderer>().ModelUUID;
            Animation initAnim(modelUUID);
            CommandHistory::Execute(std::make_unique<ComponentAddCommand<Animation>>(
                "Add Animation", e.GetScene(), (entt::entity)e, initAnim));
        };
        opts.OnCopy = [](Entity e) {
            const auto& a = e.GetComponent<Animation>();
            nlohmann::json j;
            j["modelUUID"] = a.ModelUUID.ToString();
            j["clipUUID"]  = a.ClipUUID.ToString();
            j["speed"]     = a.Speed;
            j["loopMode"]  = (int)a.LoopMode;
            j["playing"]   = a.Playing;
            return j.dump();
        };
        opts.OnPaste = [](Entity e, const std::string& data) -> bool {
            try {
                auto j = nlohmann::json::parse(data);
                Animation newA = e.GetComponent<Animation>();
                newA.ModelUUID = UUID::FromString(j.value("modelUUID", ""));
                newA.ClipUUID  = UUID::FromString(j.value("clipUUID", ""));
                newA.Speed     = j.value("speed", 1.0f);
                newA.LoopMode  = (AnimationLoopMode)j.value("loopMode", 1);
                newA.Playing   = j.value("playing", true);
                CommandHistory::Execute(std::make_unique<ComponentReplaceCommand<Animation>>(
                    "Paste Animation", e.GetScene(), (entt::entity)e, std::move(newA)));
                return true;
            } catch (...) { return false; }
        };

        ComponentDrawerRegistry::Register<Animation>(
            "Animation",
            [](Entity entity, Animation& animation) {
                if (!animation.ModelUUID.IsValid() && entity.HasComponent<MeshRenderer>()) {
                    auto& mr = entity.GetComponent<MeshRenderer>();
                    if (mr.ModelUUID.IsValid())
                        animation.ModelUUID = mr.ModelUUID;
                }

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

                // Auto-pick the first clip when the component was just added so
                // playback starts on something visible instead of bind pose.
                const auto& clipUUIDs = model->GetAnimationClipUUIDs();
                if (!animation.ClipUUID.IsValid() && !clipUUIDs.empty()) {
                    animation.ClipUUID = clipUUIDs[0];
                }

                if (UI::BeginProperties("AnimProps")) {
                    Scene* scene = entity.GetScene();
                    entt::entity ent = (entt::entity)entity;

                    {
                        auto state = UI::PropertyAsset("Clip", animation.ClipUUID, AssetType::Animation);
                        if (state.committed) {
                            animation.CurrentTime = 0.0f;
                            CommandHistory::Execute(std::make_unique<ComponentPropertyCommand<Animation, UUID>>(
                                "Change Clip", scene, ent, &Animation::ClipUUID,
                                UI::ConsumeItemPreEdit<UUID>(state.itemId), animation.ClipUUID));
                        }
                    }

                    auto clipPtr = AssetManager::GetAsset<AnimationClip>(animation.ClipUUID);
                    const AnimationClip* clip = clipPtr.get();
                    if (clip) {
                        f32 duration = clip->GetDurationSeconds();

                        {
                            auto state = UI::Property("Speed", animation.Speed, 0.05f, 0.0f, 5.0f);
                            if (state.committed)
                                CommandHistory::Execute(std::make_unique<ComponentPropertyCommand<Animation, f32>>(
                                    "Change Speed", scene, ent, &Animation::Speed,
                                    UI::ConsumeItemPreEdit<f32>(state.itemId), animation.Speed));
                        }

                        if (duration > 0.0f) {
                            auto state = UI::Property("Timeline", animation.CurrentTime, 0.01f, 0.0f, duration);
                            if (state.committed)
                                CommandHistory::Execute(std::make_unique<ComponentPropertyCommand<Animation, f32>>(
                                    "Change Timeline", scene, ent, &Animation::CurrentTime,
                                    UI::ConsumeItemPreEdit<f32>(state.itemId), animation.CurrentTime));
                        }
                    }

                    UI::Property("Show Bones", Editor::GetSettings().showBoneDebug);

                    UI::EndProperties();
                }

                auto clipPtr = AssetManager::GetAsset<AnimationClip>(animation.ClipUUID);
                const AnimationClip* clip = clipPtr.get();
                if (!clip) return;

                ImGui::Dummy({ 0, 4 });

                float buttonWidth = 32.0f;
                float spacing = 4.0f;
                float totalWidth = (buttonWidth * 3) + (spacing * 2);
                AlignItemToCenter(totalWidth);

                ImGui::BeginGroup();
                ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);
                ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(spacing, 4.0f));

                if (animation.Playing) {
                    ImGui::PushFont(Editor::GetIconFill());
                    const bool pauseClicked = ImGui::Button(ICON_PAUSE_FILL "##AnimPause", ImVec2(buttonWidth, 24));
                    ImGui::PopFont();
                    if (pauseClicked) {
                        CommandHistory::Execute(std::make_unique<ComponentPropertyCommand<Animation, bool>>(
                            "Pause Animation", entity.GetScene(), (entt::entity)entity,
                            &Animation::Playing, true, false));
                    }
                } else {
                    ImGui::PushFont(Editor::GetIconFill());
                    const bool playClicked = ImGui::Button(ICON_PLAY_FILL "##AnimPlay", ImVec2(buttonWidth, 24));
                    ImGui::PopFont();
                    if (playClicked) {
                        CommandHistory::Execute(std::make_unique<ComponentPropertyCommand<Animation, bool>>(
                            "Play Animation", entity.GetScene(), (entt::entity)entity,
                            &Animation::Playing, false, true));
                    }
                }
                ImGui::SameLine();

                ImGui::PushFont(Editor::GetIconFill());
                const bool stopClicked = ImGui::Button(ICON_STOP_FILL "##AnimStop", ImVec2(buttonWidth, 24));
                ImGui::PopFont();
                if (stopClicked) {
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

                const char* loopIcon = ICON_ARROW_RIGHT;
                const char* loopTooltip = "Loop: Off";
                if (animation.LoopMode == AnimationLoopMode::One) {
                    loopIcon = ICON_REDO;
                    loopTooltip = "Loop: One";
                }
                else if (animation.LoopMode == AnimationLoopMode::All) {
                    loopIcon = ICON_REFRESH;
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
            },
            std::move(opts));
    }
}
