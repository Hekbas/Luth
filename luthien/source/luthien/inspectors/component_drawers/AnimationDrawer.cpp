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

                const auto& clips = model->GetAnimationClips();
                int clipCount = (int)clips.size();
                if (clipCount == 0) {
                    ImGui::TextDisabled("No animation clips");
                    return;
                }

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

                    UI::Property("Show Bones", Editor::GetSettings().showBoneDebug);

                    UI::EndProperties();
                }

                const AnimationClip* clip = model->GetAnimationClip((u32)animation.AnimationIndex);
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

                const char* loopIcon = ICON_FA_ARROW_RIGHT;
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
            },
            std::move(opts));
    }
}
