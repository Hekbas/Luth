#pragma once

#include "luth/ECS/System.h"
#include "luth/core/Memory.h"
#include "luth/renderer/rendergraph/RenderGraph.h"
#include "luth/renderer/backend/vulkan/VulkanPipeline.h"
#include "luth/renderer/Texture.h"

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

        std::shared_ptr<Texture> GetSceneColor() const { return m_SceneColor; }

    private:
        // Memory
        std::unique_ptr<LinearAllocator> m_FrameAllocator;

        // Resources
        std::shared_ptr<Texture> m_SceneColor;

        // Vulkan Test Pipeline
        std::unique_ptr<VKPipeline> m_TrianglePipeline;
    };

}
