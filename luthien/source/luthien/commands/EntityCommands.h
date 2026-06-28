#pragma once

#include "luthien/commands/ICommand.h"
#include "luth/core/types/LuthMath.h"
#include "luth/core/UUID.h"
#include "luth/scene/Scene.h"
#include "luth/scene/Entity.h"

#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace Luth
{
    // Entity serialization helpers used by destroy/duplicate commands.
    namespace CommandUtil
    {
        nlohmann::json SerializeEntity(Entity entity);
        nlohmann::json SerializeEntitySubtree(Entity root);
        void DeserializeEntitySubtree(Scene& scene, const nlohmann::json& snapshot);
        i32 GetSiblingIndex(Entity entity);
        void InsertAtSiblingIndex(Scene& scene, Entity entity, Entity parent, i32 index);
    }

    // Coalesced gizmo drag: captures start/end transform.
    class GizmoTransformCommand : public ICommand
    {
    public:
        GizmoTransformCommand(Scene* scene, entt::entity entity,
                              Vec3 oldPos, Vec3 oldRot, Vec3 oldScale,
                              Vec3 newPos, Vec3 newRot, Vec3 newScale);
        void Execute() override;
        void Undo() override;
        void Redo() override;
        const char* GetName() const override { return "Gizmo Transform"; }

    private:
        void Apply(const Vec3& pos, const Vec3& rot, const Vec3& scale);
        Scene* m_Scene;
        UUID m_EntityUUID;
        Vec3 m_OldPos, m_OldRot, m_OldScale;
        Vec3 m_NewPos, m_NewRot, m_NewScale;
    };

    class EntityCreateCommand : public ICommand
    {
    public:
        EntityCreateCommand(Scene* scene, const std::string& name, UUID parentUUID = UUID::Invalid());
        void Execute() override;
        void Undo() override;
        void Redo() override;
        const char* GetName() const override { return "Create Entity"; }

        Entity GetCreatedEntity() const;

    private:
        Scene* m_Scene;
        std::string m_EntityName;
        UUID m_ParentUUID;
        UUID m_CreatedUUID;
        bool m_FirstExecution = true;
    };

    class EntityDestroyCommand : public ICommand
    {
    public:
        EntityDestroyCommand(Scene* scene, Entity entity);
        void Execute() override;
        void Undo() override;
        void Redo() override;
        const char* GetName() const override { return "Delete Entity"; }

    private:
        Scene* m_Scene;
        UUID m_EntityUUID;
        UUID m_ParentUUID;
        i32 m_SiblingIndex = -1;
        nlohmann::json m_Snapshot;
        bool m_WasSelected = false;
    };

    // Batch delete: destroys an arbitrary selected set as one undoable step, restoring both
    // the subtrees and the exact prior multi-selection. Input is root-filtered (a selected
    // parent already snapshots its selected child via SerializeEntitySubtree, so the child
    // must not be handled twice or undo would deserialize it twice).
    class EntityDestroyMultipleCommand : public ICommand
    {
    public:
        EntityDestroyMultipleCommand(Scene* scene, const std::vector<Entity>& entities);
        void Execute() override;
        void Undo() override;
        void Redo() override;
        const char* GetName() const override { return m_Name.c_str(); }

        bool IsEmpty() const { return m_Records.empty(); }

    private:
        struct Record {
            UUID uuid;
            UUID parentUUID;
            i32  siblingIndex = -1;
            nlohmann::json snapshot;
        };

        Scene* m_Scene;
        std::vector<Record> m_Records;        // sorted by sibling index asc (correct reinsert)
        std::vector<UUID>   m_PriorSelection; // ordered; primary = back()
        std::string         m_Name;
    };

    class EntityRenameCommand : public ICommand
    {
    public:
        EntityRenameCommand(Scene* scene, entt::entity entity,
                            std::string oldName, std::string newName);
        void Execute() override;
        void Undo() override;
        void Redo() override;
        const char* GetName() const override { return "Rename Entity"; }

    private:
        Scene* m_Scene;
        UUID m_EntityUUID;
        std::string m_OldName;
        std::string m_NewName;
    };

    class EntityActiveCommand : public ICommand
    {
    public:
        EntityActiveCommand(Scene* scene, entt::entity entity, bool oldValue, bool newValue);
        void Execute() override;
        void Undo() override;
        void Redo() override;
        const char* GetName() const override { return "Toggle Entity Active"; }

    private:
        Scene* m_Scene;
        UUID m_EntityUUID;
        bool m_OldValue;
        bool m_NewValue;
    };

    class EntityReparentCommand : public ICommand
    {
    public:
        EntityReparentCommand(Scene* scene, Entity entity, Entity newParent);
        void Execute() override;
        void Undo() override;
        void Redo() override;
        const char* GetName() const override { return "Reparent Entity"; }

    private:
        Scene* m_Scene;
        UUID m_EntityUUID;
        UUID m_OldParentUUID;
        UUID m_NewParentUUID;
        i32 m_OldSiblingIndex = -1;
    };

    class EntityReorderCommand : public ICommand
    {
    public:
        EntityReorderCommand(Scene* scene, Entity entity, Entity target, bool after);
        void Execute() override;
        void Undo() override;
        void Redo() override;
        const char* GetName() const override { return "Reorder Entity"; }

    private:
        Scene* m_Scene;
        UUID m_EntityUUID;
        UUID m_OldParentUUID;
        UUID m_TargetUUID;
        bool m_After;
        i32 m_OldSiblingIndex = -1;
    };

    class EntityDuplicateCommand : public ICommand
    {
    public:
        EntityDuplicateCommand(Scene* scene, Entity original);
        void Execute() override;
        void Undo() override;
        void Redo() override;
        const char* GetName() const override { return "Duplicate Entity"; }

        Entity GetDuplicatedEntity() const;

    private:
        Scene* m_Scene;
        UUID m_OriginalUUID;
        UUID m_DuplicateUUID;
        nlohmann::json m_DuplicateSnapshot;
        bool m_FirstExecution = true;
    };
}
