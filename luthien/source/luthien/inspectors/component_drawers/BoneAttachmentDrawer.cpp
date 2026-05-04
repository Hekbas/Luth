#include "lepch.h"
#include "luthien/inspectors/ComponentDrawerRegistry.h"
#include "luthien/inspectors/component_drawers/RegisterComponentDrawers.h"
#include "luthien/widgets/Widgets.h"
#include "luthien/commands/Commands.h"
#include "luthien/CommandHistory.h"
#include "luth/scene/Components.h"
#include "luth/resources/AssetManager.h"
#include "luth/renderer/resources/Model.h"

#include <nlohmann/json.hpp>

namespace Luth::ComponentDrawers
{
    using namespace Component;

    void RegisterBoneAttachment()
    {
        ComponentDrawerOptions opts;
        opts.OnCopy = [](Entity e) {
            const auto& ba = e.GetComponent<BoneAttachment>();
            nlohmann::json j;
            j["boneName"]      = ba.BoneName;
            j["localOffset"]   = { ba.LocalOffset.x,   ba.LocalOffset.y,   ba.LocalOffset.z };
            j["localRotation"] = { ba.LocalRotation.x, ba.LocalRotation.y, ba.LocalRotation.z };
            return j.dump();
        };
        // TargetEntity + BoneIndex preserved from existing component — paste copies
        // only the offset/rotation/name. Re-link target via the inspector if needed.
        opts.OnPaste = [](Entity e, const std::string& data) -> bool {
            try {
                auto j = nlohmann::json::parse(data);
                BoneAttachment newBa = e.GetComponent<BoneAttachment>();
                newBa.BoneName = j.value("boneName", "");
                if (j.contains("localOffset") && j["localOffset"].is_array() && j["localOffset"].size() >= 3)
                    newBa.LocalOffset = { j["localOffset"][0], j["localOffset"][1], j["localOffset"][2] };
                if (j.contains("localRotation") && j["localRotation"].is_array() && j["localRotation"].size() >= 3)
                    newBa.LocalRotation = { j["localRotation"][0], j["localRotation"][1], j["localRotation"][2] };
                CommandHistory::Execute(std::make_unique<ComponentReplaceCommand<BoneAttachment>>(
                    "Paste BoneAttachment", e.GetScene(), (entt::entity)e, std::move(newBa)));
                return true;
            } catch (...) { return false; }
        };

        ComponentDrawerRegistry::Register<BoneAttachment>(
            "Bone Attachment",
            [](Entity entity, BoneAttachment& attachment) {
                Scene* scene = entity.GetScene();
                if (!scene) return;

                auto& registry = scene->Registry();
                std::vector<entt::entity> animEntities;
                std::vector<std::string> entityNames;

                entityNames.push_back("None");

                int currentIndex = 0;
                auto view = registry.view<Animation, Tag>();
                for (auto e : view) {
                    animEntities.push_back(e);
                    entityNames.push_back(registry.get<Tag>(e).Value);
                    if (attachment.TargetEntity && (entt::entity)attachment.TargetEntity == e)
                        currentIndex = (int)animEntities.size();
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
                                    attachment.BoneIndex = -1;

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
                        auto state = UI::Property("Local Offset", attachment.LocalOffset);
                        if (state.committed)
                            CommandHistory::Execute(std::make_unique<ComponentPropertyCommand<BoneAttachment, Vec3>>(
                                "Change Local Offset", entity.GetScene(), (entt::entity)entity,
                                &BoneAttachment::LocalOffset,
                                UI::ConsumeItemPreEdit<Vec3>(state.itemId), attachment.LocalOffset));
                    }
                    {
                        auto state = UI::Property("Local Rotation", attachment.LocalRotation);
                        if (state.committed)
                            CommandHistory::Execute(std::make_unique<ComponentPropertyCommand<BoneAttachment, Vec3>>(
                                "Change Local Rotation", entity.GetScene(), (entt::entity)entity,
                                &BoneAttachment::LocalRotation,
                                UI::ConsumeItemPreEdit<Vec3>(state.itemId), attachment.LocalRotation));
                    }

                    UI::EndProperties();
                }
            },
            std::move(opts));
    }
}
