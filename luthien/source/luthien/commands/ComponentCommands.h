#pragma once

#include "luthien/commands/ICommand.h"
#include "luth/core/UUID.h"
#include "luth/scene/Scene.h"
#include "luth/scene/Entity.h"
#include "luth/scene/Components.h"

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
        ComponentRemoveCommand(const char* name, Scene* scene, entt::entity entity)
            : m_Name(name), m_Scene(scene)
        {
            Entity e{ entity, scene };
            m_EntityUUID = e.GetComponent<Component::ID>().Value;
        }

        void Execute() override {
            Entity e = m_Scene->FindEntityByUUID(m_EntityUUID);
            if (!e.IsValid()) return;
            if (e.HasComponent<T>()) {
                m_SavedValue = e.GetComponent<T>();
                e.RemoveComponent<T>();
            }
        }
        void Undo() override {
            Entity e = m_Scene->FindEntityByUUID(m_EntityUUID);
            if (!e.IsValid()) return;
            e.AddOrReplaceComponent<T>(m_SavedValue);
        }
        void Redo() override { Execute(); }
        const char* GetName() const override { return m_Name; }

    private:
        const char* m_Name;
        Scene* m_Scene;
        UUID m_EntityUUID;
        T m_SavedValue{};
    };

    template<typename T>
    class ComponentResetCommand : public ICommand
    {
    public:
        ComponentResetCommand(const char* name, Scene* scene, entt::entity entity)
            : m_Name(name), m_Scene(scene)
        {
            Entity e{ entity, scene };
            m_EntityUUID = e.GetComponent<Component::ID>().Value;
        }

        void Execute() override {
            Entity e = m_Scene->FindEntityByUUID(m_EntityUUID);
            if (!e.IsValid() || !e.HasComponent<T>()) return;
            m_SavedValue = e.GetComponent<T>();
            e.GetComponent<T>() = T{};
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
}
