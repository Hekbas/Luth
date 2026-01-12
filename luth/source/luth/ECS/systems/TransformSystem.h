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

        struct TransformJobData
        {
            entt::registry* registry;
            std::vector<entt::entity>* entities;
        };

        static void UpdateTransformsJob(JobSystem::JobArgs args)
        {
            TransformJobData* data = (TransformJobData*)args.data;
            entt::entity entity = (*data->entities)[args.jobIndex];
            entt::registry& reg = *data->registry;

            // Safety check: Ensure entity is valid and has required components
            if (!reg.valid(entity) || !reg.all_of<Transform, WorldTransform>(entity)) return;

            auto& transform = reg.get<Transform>(entity);
            auto& world = reg.get<WorldTransform>(entity);

            // 1. Update Local
            if (transform.IsDirty)
            {
                glm::mat4 rotation = glm::toMat4(glm::quat(glm::radians(transform.Rotation)));
                transform.LocalMatrix = glm::translate(glm::mat4(1.0f), transform.Position)
                    * rotation
                    * glm::scale(glm::mat4(1.0f), transform.Scale);
            }

            // 2. Update World
            // Parent is guaranteed to be updated because we process by levels
            if (reg.any_of<Parent>(entity))
            {
                entt::entity parent = reg.get<Parent>(entity).m_Parent;
                const auto& parentWorld = reg.get<WorldTransform>(parent);
                
                // Optimization: Check if parent changed? 
                // For now, simple matrix mult. 
                // Ideally we track IsDirty propagation, but in parallel we can't easily read parent's dirty flag 
                // if we cleared it in the previous level's job.
                // So we just recompute. Matrix mult is cheap.
                world.Matrix = parentWorld.Matrix * transform.LocalMatrix;
            }
            else
            {
                world.Matrix = transform.LocalMatrix;
            }
            
            transform.IsDirty = false;
        }

        void Update(Scene* scene) override
        {
            LH_PROFILE_FUNCTION();

            if (scene->GetHierarchyVersion() != m_LastHierarchyVersion)
            {
                RebuildHierarchy(scene);
                m_LastHierarchyVersion = scene->GetHierarchyVersion();
            }

            JobSystem::Counter counter;
            TransformJobData jobData;
            jobData.registry = &scene->Registry();

            for (auto& level : m_Levels)
            {
                if (level.empty()) continue;
                
                jobData.entities = &level;
                // Group size 64 seems reasonable for matrix mults
                JobSystem::Dispatch((u32)level.size(), 64, UpdateTransformsJob, &jobData, &counter);
                JobSystem::WaitForCounter(&counter);
            }
        }

    private:
        void RebuildHierarchy(Scene* scene)
        {
            LH_PROFILE_FUNCTION();
            m_Levels.clear();
            
            // Level 0: Roots
            m_Levels.push_back({});
            for (auto entity : scene->GetRootEntities())
                m_Levels[0].push_back((entt::entity)entity);

            // BFS
            size_t currentLevel = 0;
            entt::registry& reg = scene->Registry();

            while (true)
            {
                std::vector<entt::entity> nextLevel;
                // Reserve optimization?
                
                for (auto parent : m_Levels[currentLevel])
                {
                    if (reg.any_of<Children>(parent))
                    {
                        const auto& children = reg.get<Children>(parent).m_Children;
                        for (auto child : children)
                            nextLevel.push_back((entt::entity)child);
                    }
                }

                if (nextLevel.empty()) break;
                m_Levels.push_back(std::move(nextLevel));
                currentLevel++;
            }
        }

        std::vector<std::vector<entt::entity>> m_Levels;
        u32 m_LastHierarchyVersion = 0;
    };
}
