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

        void Update(entt::registry& registry) override
        {
            LH_PROFILE_FUNCTION();

            auto view = registry.view<Transform, WorldTransform>();
            
            // Convert view to a vector of entities for parallel processing
            // Note: In a true SoA ECS, we wouldn't need this copy step.
            // For now, we pay the copy cost to gain parallelism.
            std::vector<entt::entity> entities;
            entities.reserve(view.size_hint());
            for (auto entity : view)
                entities.push_back(entity);

            if (entities.empty()) return;

            JobSystem::Counter counter;
            
            // Process 64 transforms per job
            JobSystem::Dispatch((u32)entities.size(), 64, [&](JobSystem::JobArgs args)
            {
                entt::entity entity = entities[args.jobIndex];
                
                // Thread-safe access? 
                // entt::registry is NOT thread-safe for writing, but we are writing to 
                // specific components (WorldTransform) that are unique per job.
                // Reading (Transform, Parent) is safe if no structural changes happen.
                
                auto& transform = view.get<Transform>(entity);
                auto& worldTransform = view.get<WorldTransform>(entity);

                // Calculate Local Matrix
                glm::mat4 world = transform.GetTransform();

                // Parent Hierarchy Walk
                // TODO: This is O(Depth) per entity. 
                // Optimization: Sort by depth and compute parents first.
                if (registry.any_of<Parent>(entity))
                {
                    entt::entity current = registry.get<Parent>(entity).m_Parent;
                    while (registry.valid(current) && registry.any_of<Transform>(current)) 
                    {
                        const auto& t = registry.get<Transform>(current);
                        world = t.GetTransform() * world;

                        if (!registry.any_of<Parent>(current))
                            break;

                        current = registry.get<Parent>(current).m_Parent;
                    }
                }

                worldTransform.matrix = world;

            }, &counter);

            JobSystem::WaitForCounter(&counter);
        }
    };
}
