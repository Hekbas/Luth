#pragma once

#include "luth/scene/systems/ISystem.h"
#include "luth/scene/Components.h"
#include "luth/jobs/JobSystem.h"
#include "luth/core/Profiler.h"

namespace Luth
{
    class TransformSystem : public ISystem
    {
    public:
        TransformSystem() {}

        struct TransformJobData
        {
            entt::registry* registry;
            std::vector<entt::entity>* entities;
            u32 totalCount;
            u32 groupSize;
        };

        static void UpdateTransformsJob(JobSystem::JobArgs args)
        {
            TransformJobData* data = (TransformJobData*)args.data;
            entt::registry& reg = *data->registry;

            // Dispatch creates one job per group. Each job processes a range of entities.
            u32 start = args.jobIndex;
            u32 end = std::min(start + data->groupSize, data->totalCount);

            for (u32 i = start; i < end; i++)
            {
                entt::entity entity = (*data->entities)[i];

                // Safety check: Ensure entity is valid and has required components
                if (!reg.valid(entity) || !reg.all_of<Component::Transform, Component::WorldTransform>(entity)) continue;

                auto& transform = reg.get<Component::Transform>(entity);
                auto& world = reg.get<Component::WorldTransform>(entity);

                // 1. Update Local
                if (transform.IsDirty)
                {
                    Mat4 rotation = Math::ToMat4(Quat(Math::Radians(transform.Rotation)));
                    transform.LocalMatrix = Math::Translate(Mat4(1.0f), transform.Position)
                        * rotation
                        * Math::Scale(Mat4(1.0f), transform.Scale);
                }

                // 2. Update World
                // Parent is guaranteed to be updated because we process by levels
                if (reg.any_of<Component::Parent>(entity))
                {
                    entt::entity parent = reg.get<Component::Parent>(entity).Value;
                    const auto& parentWorld = reg.get<Component::WorldTransform>(parent);
                    world.Matrix = parentWorld.Matrix * transform.LocalMatrix;
                }
                else
                {
                    world.Matrix = transform.LocalMatrix;
                }

                transform.IsDirty = false;
            }
        }

        void Update(Scene* scene) override
        {
            LH_PROFILE_FUNCTION();

            // Rebuild if version changed OR if we have roots but no levels (first frame init)
            if (scene->GetHierarchyVersion() != m_LastHierarchyVersion || (m_Levels.empty() && !scene->GetRootEntities().empty()))
            {
                RebuildHierarchy(scene);
                m_LastHierarchyVersion = scene->GetHierarchyVersion();
            }

            JobSystem::Counter counter;
            TransformJobData jobData;
            jobData.registry = &scene->Registry();
            jobData.groupSize = 64;

            for (auto& level : m_Levels)
            {
                if (level.empty()) continue;

                jobData.entities = &level;
                jobData.totalCount = (u32)level.size();
                JobSystem::Dispatch(jobData.totalCount, jobData.groupSize, UpdateTransformsJob, &jobData, &counter);
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
                    if (reg.any_of<Component::Children>(parent))
                    {
                        const auto& children = reg.get<Component::Children>(parent).Value;
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
