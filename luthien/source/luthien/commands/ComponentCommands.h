#pragma once

#include "luthien/commands/ICommand.h"
#include "luthien/MultiEdit.h"
#include "luth/core/UUID.h"
#include "luth/scene/Scene.h"
#include "luth/scene/Entity.h"
#include "luth/scene/Components.h"

#include <utility>
#include <vector>

namespace Luth
{
    // Add / remove / property-change commands for ECS components. Each is keyed by Component::ID
    // (the UUID), so undo / redo survives entity-handle recycling: replaying an ancient command
    // after a destroy / recreate cycle still finds the right entity by its persistent UUID.
    template<typename T>
    class ComponentAddCommand : public ICommand
    {
    public:
        ComponentAddCommand(const char* name, Scene* scene, entt::entity entity)
            : m_Name(name), m_Scene(scene)
        {
            Entity e{ entity, scene };
            m_EntityUUID = e.GetComponent<Component::ID>().Value;
        }

        ComponentAddCommand(const char* name, Scene* scene, entt::entity entity, T initValue)
            : m_Name(name), m_Scene(scene),
              m_InitValue(std::move(initValue)), m_HasInitValue(true)
        {
            Entity e{ entity, scene };
            m_EntityUUID = e.GetComponent<Component::ID>().Value;
        }

        void Execute() override {
            Entity e = m_Scene->FindEntityByUUID(m_EntityUUID);
            if (!e.IsValid()) return;
            if (m_HasInitValue)
                e.AddOrReplaceComponent<T>(m_InitValue);
            else
                e.AddOrReplaceComponent<T>();
        }
        void Undo() override {
            Entity e = m_Scene->FindEntityByUUID(m_EntityUUID);
            if (!e.IsValid()) return;
            if (e.HasComponent<T>())
                e.RemoveComponent<T>();
        }
        void Redo() override { Execute(); }
        const char* GetName() const override { return m_Name; }

    private:
        const char* m_Name;
        Scene* m_Scene;
        UUID m_EntityUUID;
        T m_InitValue{};
        bool m_HasInitValue = false;
    };

    template<typename T>
    class ComponentRemoveCommand : public ICommand
    {
    public:
        ComponentRemoveCommand(const char* name, Scene* scene, entt::entity entity,
                               std::vector<UUID> extraTargets = {})
            : m_Name(name), m_Scene(scene)
        {
            Entity e{ entity, scene };
            m_EntityUUID = e.GetComponent<Component::ID>().Value;

            // Multi-edit: remove from the rest of the selection too (those that have T). Add/remove
            // fire on_construct/on_destroy, so physics rebuild is handled without an explicit patch.
            if (extraTargets.empty()) extraTargets = MultiEdit::Targets();
            ForEachExtraTarget<T>(scene, extraTargets, [&](Entity t) {
                m_TargetSaved.emplace_back(t.GetComponent<Component::ID>().Value, T{});
            });
        }

        void Execute() override {
            SaveAndRemove(m_Scene->FindEntityByUUID(m_EntityUUID), m_SavedValue);
            for (auto& [uuid, saved] : m_TargetSaved)
                SaveAndRemove(m_Scene->FindEntityByUUID(uuid), saved);
        }
        void Undo() override {
            Restore(m_Scene->FindEntityByUUID(m_EntityUUID), m_SavedValue);
            for (auto& [uuid, saved] : m_TargetSaved)
                Restore(m_Scene->FindEntityByUUID(uuid), saved);
        }
        void Redo() override { Execute(); }
        const char* GetName() const override { return m_Name; }

    private:
        void SaveAndRemove(Entity e, T& saveSlot) {
            if (!e.IsValid() || !e.HasComponent<T>()) return;
            saveSlot = e.GetComponent<T>();
            e.RemoveComponent<T>();
        }
        void Restore(Entity e, const T& value) {
            if (!e.IsValid()) return;
            e.AddOrReplaceComponent<T>(value);
        }

        const char* m_Name;
        Scene* m_Scene;
        UUID m_EntityUUID;
        T m_SavedValue{};
        std::vector<std::pair<UUID, T>> m_TargetSaved;
    };

    template<typename T>
    class ComponentResetCommand : public ICommand
    {
    public:
        ComponentResetCommand(const char* name, Scene* scene, entt::entity entity,
                              std::vector<UUID> extraTargets = {})
            : m_Name(name), m_Scene(scene)
        {
            Entity e{ entity, scene };
            m_EntityUUID = e.GetComponent<Component::ID>().Value;

            // Multi-edit: reset the component on the rest of the selection too.
            if (extraTargets.empty()) extraTargets = MultiEdit::Targets();
            ForEachExtraTarget<T>(scene, extraTargets, [&](Entity t) {
                m_TargetSaved.emplace_back(t.GetComponent<Component::ID>().Value, T{});
            });
        }

        void Execute() override {
            SaveAndReset(m_Scene->FindEntityByUUID(m_EntityUUID), m_SavedValue);
            for (auto& [uuid, saved] : m_TargetSaved)
                SaveAndReset(m_Scene->FindEntityByUUID(uuid), saved);
        }
        void Undo() override {
            Restore(m_Scene->FindEntityByUUID(m_EntityUUID), m_SavedValue);
            for (auto& [uuid, saved] : m_TargetSaved)
                Restore(m_Scene->FindEntityByUUID(uuid), saved);
        }
        void Redo() override { Execute(); }
        const char* GetName() const override { return m_Name; }

    private:
        void SaveAndReset(Entity e, T& saveSlot) {
            if (!e.IsValid() || !e.HasComponent<T>()) return;
            saveSlot = e.GetComponent<T>();
            e.GetComponent<T>() = T{};
            m_Scene->Registry().patch<T>((entt::entity)e);   // physics rebuild (no-op elsewhere)
        }
        void Restore(Entity e, const T& value) {
            if (!e.IsValid() || !e.HasComponent<T>()) return;
            e.GetComponent<T>() = value;
            m_Scene->Registry().patch<T>((entt::entity)e);
        }

        const char* m_Name;
        Scene* m_Scene;
        UUID m_EntityUUID;
        T m_SavedValue{};
        std::vector<std::pair<UUID, T>> m_TargetSaved;
    };

    template<typename T>
    class ComponentReplaceCommand : public ICommand
    {
    public:
        ComponentReplaceCommand(const char* name, Scene* scene, entt::entity entity, T newValue)
            : m_Name(name), m_Scene(scene), m_NewValue(std::move(newValue))
        {
            Entity e{ entity, scene };
            m_EntityUUID = e.GetComponent<Component::ID>().Value;
        }

        void Execute() override {
            Entity e = m_Scene->FindEntityByUUID(m_EntityUUID);
            if (!e.IsValid() || !e.HasComponent<T>()) return;
            m_SavedValue = e.GetComponent<T>();
            e.GetComponent<T>() = m_NewValue;
        }
        void Undo() override {
            Entity e = m_Scene->FindEntityByUUID(m_EntityUUID);
            if (!e.IsValid() || !e.HasComponent<T>()) return;
            e.GetComponent<T>() = m_SavedValue;
        }
        void Redo() override { Execute(); }
        const char* GetName() const override { return m_Name; }

    private:
        const char* m_Name;
        Scene* m_Scene;
        UUID m_EntityUUID;
        T m_SavedValue{};
        T m_NewValue{};
    };

    // Two-snapshot replace. Caller supplies both pre- and post-edit values explicitly. Useful when
    // ComponentPropertyCommand can't be used (e.g. union members with no stable pointer-to-member)
    // and ComponentReplaceCommand can't either (caller has already mutated the live component, so
    // the "save current at Execute time" pattern would capture the new value).
    template<typename T>
    class ComponentSnapshotCommand : public ICommand
    {
    public:
        ComponentSnapshotCommand(const char* name, Scene* scene, entt::entity entity, T oldValue, T newValue)
            : m_Name(name), m_Scene(scene), m_Old(std::move(oldValue)), m_New(std::move(newValue))
        {
            Entity e{ entity, scene };
            m_EntityUUID = e.GetComponent<Component::ID>().Value;
        }

        void Execute() override { Apply(m_New); }
        void Undo()    override { Apply(m_Old); }
        void Redo()    override { Apply(m_New); }
        const char* GetName() const override { return m_Name; }

    private:
        void Apply(const T& v) {
            Entity e = m_Scene->FindEntityByUUID(m_EntityUUID);
            if (!e.IsValid() || !e.HasComponent<T>()) return;
            e.GetComponent<T>() = v;
        }

        const char* m_Name;
        Scene* m_Scene;
        UUID m_EntityUUID;
        T m_Old{};
        T m_New{};
    };
}
