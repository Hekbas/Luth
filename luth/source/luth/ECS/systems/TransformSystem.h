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

        struct UpdateData
        {
            entt::registry* registry;
            std::vector<entt::entity>* entities;
        };

        static void UpdateTransformJob(JobSystem::JobArgs args)
        {
            UpdateData* data = (UpdateData*)args.data;
            entt::registry& registry = *data->registry;
            entt::entity entity = (*data->entities)[args.jobIndex];

            // We need to get components manually since we can't capture the view
            // Optimization: Pass raw pointers to component arrays if possible
            auto& transform = registry.get<Transform>(entity);
            auto& worldTransform = registry.get<WorldTransform>(entity);

            glm::mat4 world = transform.GetTransform();

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
        }

        void Update(entt::registry& registry) override
        {
            LH_PROFILE_FUNCTION();

            auto view = registry.view<Transform, WorldTransform>();
            
            // Allocate entities vector on stack or frame allocator?
            // std::vector allocates on heap.
            // For now, heap is fine for the vector itself, but we should use FrameAllocator later.
            std::vector<entt::entity> entities;
            entities.reserve(view.size_hint());
            for (auto entity : view)
                entities.push_back(entity);

            if (entities.empty()) return;

            UpdateData jobData;
            jobData.registry = &registry;
            jobData.entities = &entities;

            JobSystem::Counter counter;
            JobSystem::Dispatch((u32)entities.size(), 64, UpdateTransformJob, &jobData, &counter);
            JobSystem::WaitForCounter(&counter);
        }
    };
}
