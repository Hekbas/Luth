#pragma once

#include "luth/ECS/System.h"
#include "luth/ECS/Components.h"
#include "luth/core/JobSystem.h"
#include "luth/core/Profiler.h"

namespace Luth
{
    class TransformSystem : public System
    {
    public:
        TransformSystem() {}

        void UpdateEntityAndChildren(entt::registry& registry, entt::entity entity, const glm::mat4& parentTransform, bool parentDirty)
        {
            auto& transform = registry.get<Transform>(entity);
            auto& worldTransform = registry.get<WorldTransform>(entity);

            // 1. Update Local Matrix if dirty
            if (transform.IsDirty) 
            {
                glm::mat4 rotation = glm::toMat4(glm::quat(glm::radians(transform.Rotation)));
                transform.LocalMatrix = glm::translate(glm::mat4(1.0f), transform.Position)
                    * rotation
                    * glm::scale(glm::mat4(1.0f), transform.Scale);
            }

            // 2. Update World Matrix
            // If parent was dirty, we must update world even if we aren't dirty
            if (transform.IsDirty || parentDirty)
            {
                worldTransform.Matrix = parentTransform * transform.LocalMatrix;
                transform.IsDirty = false; // Clear flag now that world is updated
                parentDirty = true; // Propagate dirty state to children
            }

            // 3. Recurse to Children
            if (registry.any_of<Children>(entity))
            {
                const auto& children = registry.get<Children>(entity).m_Children;
                for (auto child : children)
                {
                    if (child.IsValid()) // Check validity
                    {
                        UpdateEntityAndChildren(registry, (entt::entity)child, worldTransform.Matrix, parentDirty);
                    }
                }
            }
        }

        void Update(entt::registry& registry) override
        {
            LH_PROFILE_FUNCTION();

            // Iterate Root Entities (those without Parent component)
            // We need a way to get roots efficiently. Scene maintains m_RootEntities but System doesn't have access to Scene class directly, only registry.
            // We can iterate all entities with Transform but NO Parent.
            
            auto view = registry.view<Transform>(entt::exclude<Parent>);
            glm::mat4 identity(1.0f);

            for (auto entity : view)
            {
                UpdateEntityAndChildren(registry, entity, identity, false);
            }
        }
    };
}
