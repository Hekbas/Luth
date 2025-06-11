#pragma once

#include "luth/ECS/System.h"
#include "luth/ECS/Components.h"

namespace Luth
{
    class TransformSystem : public System
    {
    public:
        TransformSystem() {}

        void Update(entt::registry& registry) override
        {
            auto view = registry.view<Transform, WorldTransform>();
            for (auto [entity, transform, worldTransform] : view.each()) {
                glm::mat4 world = glm::mat4(1.0f);
                entt::entity current = entity;

                while (registry.valid(current) && registry.any_of<Transform>(current)) {
                    const auto& t = registry.get<Transform>(current);
                    world = t.GetTransform() * world;

                    if (!registry.any_of<Parent>(current))
                        break;

                    current = registry.get<Parent>(current).m_Parent;
                }

                worldTransform.matrix = world;
            }
        }
    };
}
