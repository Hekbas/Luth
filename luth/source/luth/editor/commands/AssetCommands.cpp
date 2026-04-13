#include "luthpch.h"
#include "luth/editor/commands/AssetCommands.h"
#include "luth/editor/commands/EntityCommands.h"
#include "luth/scene/Scene.h"
#include "luth/scene/Components.h"
#include "luth/renderer/Material.h"
#include "luth/renderer/Model.h"
#include "luth/resources/AssetManager.h"

namespace Luth
{
    using namespace Component;

    // ═══════════════════════════════════════════════════════════════════════════
    //  MaterialSnapshotCommand
    // ═══════════════════════════════════════════════════════════════════════════

    MaterialSnapshotCommand::MaterialSnapshotCommand(UUID materialUUID,
                                                     nlohmann::json oldState,
                                                     nlohmann::json newState)
        : m_MaterialUUID(materialUUID),
          m_OldState(std::move(oldState)),
          m_NewState(std::move(newState)) {}

    void MaterialSnapshotCommand::Execute() { ApplyState(m_NewState); }
    void MaterialSnapshotCommand::Undo()    { ApplyState(m_OldState); }
    void MaterialSnapshotCommand::Redo()    { ApplyState(m_NewState); }

    void MaterialSnapshotCommand::ApplyState(const nlohmann::json& state)
    {
        auto mat = AssetManager::GetAsset<Material>(m_MaterialUUID);
        if (!mat) return;
        mat->Deserialize(state);
        mat->MarkDirty();
    }

    // ═══════════════════════════════════════════════════════════════════════════
    //  ModelInstantiateCommand
    // ═══════════════════════════════════════════════════════════════════════════

    ModelInstantiateCommand::ModelInstantiateCommand(Scene* scene, UUID modelUUID, UUID parentUUID)
        : m_Scene(scene), m_ModelUUID(modelUUID), m_ParentUUID(parentUUID) {}

    void ModelInstantiateCommand::Execute()
    {
        if (!m_FirstExecution) {
            Redo();
            return;
        }

        auto model = AssetManager::GetAsset<Model>(m_ModelUUID);
        if (!model) return;

        Entity root = m_Scene->CreateEntity(model->GetName());
        m_RootUUID = root.GetComponent<ID>().m_ID;

        if (m_ParentUUID.IsValid()) {
            Entity parent = m_Scene->FindEntityByUUID(m_ParentUUID);
            if (parent.IsValid()) root.SetParent(parent);
        }

        if (model->IsSkinned())
            root.AddComponent<Animation>(m_ModelUUID);

        const auto& meshes = model->GetMeshes();
        for (size_t i = 0; i < meshes.size(); i++) {
            Entity child = m_Scene->CreateEntity(model->GetCachedModelInfo().Meshes[i].Name);
            child.SetParent(root);
            auto& mr = child.AddComponent<MeshRenderer>();
            mr.ModelUUID = m_ModelUUID;
            mr.MeshIndex = (u32)i;
            mr.isSkinned = model->IsSkinned();
            if (i < model->GetMaterials().size()) {
                mr.MaterialUUID = model->GetMaterials()[i];
                if (mr.MaterialUUID.IsValid())
                    AssetManager::LoadAsync(mr.MaterialUUID);
            }
        }

        if (model->IsSkinned() && !model->GetSkeleton().IsEmpty()) {
            const auto& skeleton = model->GetSkeleton();
            u32 boneCount = skeleton.BoneCount();
            std::vector<Entity> boneEntities(boneCount);

            for (u32 i = 0; i < boneCount; i++) {
                const auto& bone = skeleton.Bones[i];
                Entity boneEntity = m_Scene->CreateEntity(bone.Name);
                boneEntities[i] = boneEntity;

                if (bone.ParentIndex >= 0 && bone.ParentIndex < (i32)boneCount)
                    boneEntity.SetParent(boneEntities[bone.ParentIndex]);
                else
                    boneEntity.SetParent(root);
            }
        }

        m_SubtreeSnapshot = CommandUtil::SerializeEntitySubtree(root);
        m_FirstExecution = false;
    }

    void ModelInstantiateCommand::Undo()
    {
        Entity root = m_Scene->FindEntityByUUID(m_RootUUID);
        if (root.IsValid())
            m_Scene->DestroyEntity(root);
    }

    void ModelInstantiateCommand::Redo()
    {
        CommandUtil::DeserializeEntitySubtree(*m_Scene, m_SubtreeSnapshot);
    }

    Entity ModelInstantiateCommand::GetRootEntity() const
    {
        return m_Scene->FindEntityByUUID(m_RootUUID);
    }
}
