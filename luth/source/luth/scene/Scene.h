#pragma once

#include <string>
#include <entt/entt.hpp>

namespace Luth
{
    class Entity;

    class Scene
    {
    public:
        Scene();
        ~Scene();

        Entity CreateEntity(const std::string& name = "Entity");
        void DestroyEntity(Entity entity);
        Entity DuplicateEntity(Entity original);

        entt::registry& Registry() { return m_Registry; }
        const entt::registry& Registry() const { return m_Registry; }

        template<typename Func>
        void EachEntity(Func func) {
            auto view = m_Registry.view<entt::entity>();
            view.each([this, func](entt::entity entityID) {
                func(Entity{ entityID, this });
            });
        }

        template<typename... Components>
        auto GetAllEntitiesWith() { return m_Registry.view<Components...>(); }

    private:
        std::string GenerateUniqueName(const std::string& originalName);

    private:
        entt::registry m_Registry;
    };
}
