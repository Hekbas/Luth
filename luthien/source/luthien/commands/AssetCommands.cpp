#include "lepch.h"
#include "luthien/commands/AssetCommands.h"
#include "luthien/commands/EntityCommands.h"
#include "luth/scene/Scene.h"
#include "luth/scene/Components.h"
#include "luth/renderer/material/Material.h"
#include "luth/renderer/resources/Model.h"
#include "luth/resources/AssetManager.h"

namespace Luth
{
    using namespace Component;

    // ---- MaterialSnapshotCommand ----

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

    // ---- ModelInstantiateCommand ----

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

        // Entity construction lives engine-side (Scene::InstantiateModel); the command only resolves
        // the parent, snapshots the resulting subtree for undo, and tracks the root UUID.
        Entity parent = m_ParentUUID.IsValid() ? m_Scene->FindEntityByUUID(m_ParentUUID) : Entity{};
        Entity root = m_Scene->InstantiateModel(model, parent);
        if (!root.IsValid()) return;

        m_RootUUID = root.GetComponent<ID>().Value;
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
