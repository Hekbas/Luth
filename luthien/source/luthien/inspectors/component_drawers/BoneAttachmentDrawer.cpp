#include "lepch.h"
#include "luthien/inspectors/ComponentDrawerRegistry.h"
#include "luthien/inspectors/component_drawers/RegisterComponentDrawers.h"
#include "luthien/widgets/Widgets.h"
#include "luthien/commands/Commands.h"
#include "luthien/CommandHistory.h"
#include "luth/scene/Components.h"
#include "luth/resources/AssetManager.h"
#include "luth/renderer/resources/Model.h"

namespace Luth::ComponentDrawers
{
    using namespace Component;

    void RegisterBoneAttachment()
    {
        ComponentDrawerRegistry::RegisterSimple<BoneAttachment>(
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
    }
}
