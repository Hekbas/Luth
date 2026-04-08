#pragma once

#include "luth/core/Math.h"
#include "luth/core/UUID.h"
#include "luth/scene/Scene.h"
#include "luth/scene/Entity.h"
#include "luth/scene/Components.h"

#include <memory>
#include <vector>
#include <string>
#include <nlohmann/json.hpp>

namespace Luth
{
    // ── ICommand ─────────────────────────────────────────────────

    class ICommand
    {
    public:
        virtual ~ICommand() = default;
        virtual void Execute() = 0;
        virtual void Undo() = 0;
        virtual void Redo() { Execute(); }
        virtual const char* GetName() const = 0;

        // For coalescing continuous edits (gizmo drags, sliders)
        virtual bool CanMerge(const ICommand&) const { return false; }
        virtual void MergeWith(const ICommand&) {}
    };

    // ── CompoundCommand ──────────────────────────────────────────

    class CompoundCommand : public ICommand
    {
    public:
        CompoundCommand(const char* name, std::vector<std::unique_ptr<ICommand>> cmds)
            : m_Name(name), m_Commands(std::move(cmds)) {}

        void Execute() override {
            for (auto& cmd : m_Commands) cmd->Execute();
        }
        void Undo() override {
            for (auto it = m_Commands.rbegin(); it != m_Commands.rend(); ++it)
                (*it)->Undo();
        }
        void Redo() override {
            for (auto& cmd : m_Commands) cmd->Redo();
        }
        const char* GetName() const override { return m_Name; }
        bool IsEmpty() const { return m_Commands.empty(); }

    private:
        const char* m_Name;
        std::vector<std::unique_ptr<ICommand>> m_Commands;
    };

    // ── ComponentPropertyCommand<C, T> ───────────────────────────
    // Generic: change one member of component C on an entity.
    // Uses pointer-to-member so the reference is re-resolved on undo/redo
    // (safe across EnTT pool relocations).

    template<typename C, typename T>
    class ComponentPropertyCommand : public ICommand
    {
    public:
        ComponentPropertyCommand(const char* name, Scene* scene, entt::entity entity,
                                 T C::*member, T oldValue, T newValue)
            : m_Name(name), m_Scene(scene), m_Entity(entity),
              m_Member(member), m_OldValue(std::move(oldValue)), m_NewValue(std::move(newValue)) {}

        void Execute() override { Apply(m_NewValue); }
        void Undo()    override { Apply(m_OldValue); }
        void Redo()    override { Apply(m_NewValue); }
        const char* GetName() const override { return m_Name; }

        bool CanMerge(const ICommand& other) const override {
            auto* o = dynamic_cast<const ComponentPropertyCommand<C, T>*>(&other);
            return o && o->m_Entity == m_Entity && o->m_Member == m_Member;
        }
        void MergeWith(const ICommand& other) override {
            m_NewValue = static_cast<const ComponentPropertyCommand<C, T>&>(other).m_NewValue;
        }

    private:
        void Apply(const T& value) {
            auto& comp = m_Scene->Registry().get<C>(m_Entity);
            comp.*m_Member = value;
            if constexpr (requires(C c) { c.IsDirty = true; }) {
                comp.IsDirty = true;
            }
        }

        const char* m_Name;
        Scene* m_Scene;
        entt::entity m_Entity;
        T C::*m_Member;
        T m_OldValue;
        T m_NewValue;
    };

    // ── GizmoTransformCommand ────────────────────────────────────
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
        entt::entity m_Entity;
        Vec3 m_OldPos, m_OldRot, m_OldScale;
        Vec3 m_NewPos, m_NewRot, m_NewScale;
    };

    // ── Entity Lifecycle Commands ────────────────────────────────

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
        entt::entity m_Entity;
        std::string m_OldName;
        std::string m_NewName;
    };

    // ── Hierarchy Commands ───────────────────────────────────────

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

    // ── Component Add/Remove Commands ────────────────────────────

    template<typename T>
    class ComponentAddCommand : public ICommand
    {
    public:
        ComponentAddCommand(const char* name, Scene* scene, entt::entity entity)
            : m_Name(name), m_Scene(scene), m_Entity(entity) {}

        ComponentAddCommand(const char* name, Scene* scene, entt::entity entity, T initValue)
            : m_Name(name), m_Scene(scene), m_Entity(entity),
              m_InitValue(std::move(initValue)), m_HasInitValue(true) {}

        void Execute() override {
            Entity e{ m_Entity, m_Scene };
            if (m_HasInitValue)
                e.AddOrReplaceComponent<T>(m_InitValue);
            else
                e.AddOrReplaceComponent<T>();
        }
        void Undo() override {
            Entity e{ m_Entity, m_Scene };
            if (e.HasComponent<T>())
                e.RemoveComponent<T>();
        }
        void Redo() override { Execute(); }
        const char* GetName() const override { return m_Name; }

    private:
        const char* m_Name;
        Scene* m_Scene;
        entt::entity m_Entity;
        T m_InitValue{};
        bool m_HasInitValue = false;
    };

    template<typename T>
    class ComponentRemoveCommand : public ICommand
    {
    public:
        ComponentRemoveCommand(const char* name, Scene* scene, entt::entity entity)
            : m_Name(name), m_Scene(scene), m_Entity(entity) {}

        void Execute() override {
            Entity e{ m_Entity, m_Scene };
            if (e.HasComponent<T>()) {
                m_SavedValue = e.GetComponent<T>();
                e.RemoveComponent<T>();
            }
        }
        void Undo() override {
            Entity e{ m_Entity, m_Scene };
            e.AddOrReplaceComponent<T>(m_SavedValue);
        }
        void Redo() override { Execute(); }
        const char* GetName() const override { return m_Name; }

    private:
        const char* m_Name;
        Scene* m_Scene;
        entt::entity m_Entity;
        T m_SavedValue{};
    };

    // ── Material Snapshot Command ────────────────────────────────

    class MaterialSnapshotCommand : public ICommand
    {
    public:
        MaterialSnapshotCommand(UUID materialUUID, nlohmann::json oldState, nlohmann::json newState);
        void Execute() override;
        void Undo() override;
        void Redo() override;
        const char* GetName() const override { return "Edit Material"; }

    private:
        void ApplyState(const nlohmann::json& state);
        UUID m_MaterialUUID;
        nlohmann::json m_OldState;
        nlohmann::json m_NewState;
    };

    // ── Model Instantiate Command ────────────────────────────────

    class ModelInstantiateCommand : public ICommand
    {
    public:
        ModelInstantiateCommand(Scene* scene, UUID modelUUID, UUID parentUUID);
        void Execute() override;
        void Undo() override;
        void Redo() override;
        const char* GetName() const override { return "Instantiate Model"; }

        Entity GetRootEntity() const;

    private:
        Scene* m_Scene;
        UUID m_ModelUUID;
        UUID m_ParentUUID;
        UUID m_RootUUID;
        nlohmann::json m_SubtreeSnapshot;
        bool m_FirstExecution = true;
    };

    // ── Serialization Helpers ────────────────────────────────────
    // Extracted from SceneSerializer for command use.

    namespace CommandUtil
    {
        nlohmann::json SerializeEntity(Entity entity);
        nlohmann::json SerializeEntitySubtree(Entity root);
        void DeserializeEntitySubtree(Scene& scene, const nlohmann::json& snapshot);
        i32 GetSiblingIndex(Entity entity);
        void InsertAtSiblingIndex(Scene& scene, Entity entity, Entity parent, i32 index);
    }
}
