#pragma once

#include "luth/ECS/System.h"
#include "luth/core/Memory.h"
#include "luth/renderer/rendergraph/RenderGraph.h"
#include "luth/renderer/backend/vulkan/VulkanPipeline.h"

#include <entt/entt.hpp>
#include <unordered_map>

namespace Luth
{
    class RenderingSystem : public System
    {
    public:
        RenderingSystem(u32 viewportWidth = 1280, u32 viewportHeight = 720);
        ~RenderingSystem();

        void Update(entt::registry& registry) override;
        void Resize(u32 width, u32 height);

    private:
        // Memory
        std::unique_ptr<LinearAllocator> m_FrameAllocator;

        // Vulkan Test Pipeline
        std::unique_ptr<VKPipeline> m_TrianglePipeline;
    };

}
