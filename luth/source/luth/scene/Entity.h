#pragma once

#include "luth/scene/Scene.h"

#include <entt/entt.hpp>

namespace Luth
{
    class Entity
    {
    public:
        Entity() = default;
        Entity(entt::entity handle, Scene* scene);

        template<typename T, typename... Args>
        T& AddComponent(Args&&... args) {
            LH_CORE_ASSERT(!HasComponent<T>(), "Entity already has component!");
            return m_Scene->Registry().emplace<T>(m_EntityHandle, std::forward<Args>(args)...);
        }

        template<typename T>
        T& GetComponent() const {
            LH_CORE_ASSERT(HasComponent<T>(), "Entity does not have component!");
            return m_Scene->Registry().get<T>(m_EntityHandle);
        }

        template<typename T>
        T& GetComponent() {
            LH_CORE_ASSERT(HasComponent<T>(), "Entity does not have component!");
            return m_Scene->Registry().get<T>(m_EntityHandle);
        }

        template<typename T>
        bool HasComponent() const {
            return m_Scene->Registry().all_of<T>(m_EntityHandle);
        }

        template<typename T>
        void RemoveComponent() {
            LH_CORE_ASSERT(HasComponent<T>(), "Entity does not have component!");
            m_Scene->Registry().remove<T>(m_EntityHandle);
        }

        bool IsValid() const {
            return m_Scene && m_Scene->Registry().valid(m_EntityHandle);
        }

        std::string GetName() const;
        void SetName(const std::string& name);
        void BeginRename() { m_IsRenaming = true; }
        bool IsRenaming() const { return m_IsRenaming; }

        void SetParent(Entity parent);
        Entity GetParent() const;
        void RemoveParent() { SetParent({}); }
        bool HasParent() const { return GetParent().operator bool(); }
        bool IsDescendantOf(Entity potentialAncestor) const;

        std::vector<Entity> GetChildren() const;

        bool operator==(const Entity& other) const {
            return m_EntityHandle == other.m_EntityHandle && m_Scene == other.m_Scene;
        }
        bool operator!=(const Entity& other) const { return !(*this == other); }

        operator bool() const { return m_EntityHandle != entt::null && m_Scene != nullptr; }
        operator entt::entity() const { return m_EntityHandle; }

    private:
        entt::entity m_EntityHandle{ entt::null };
        Scene* m_Scene = nullptr;
        bool m_IsRenaming = false;
    };
}
