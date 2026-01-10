#pragma once

#include "luth/ECS/System.h"
#include "luth/core/Memory.h"
#include "luth/renderer/rendergraph/RenderGraph.h"
#include "luth/renderer/backend/vulkan/VulkanPipeline.h"
#include "luth/renderer/backend/vulkan/VulkanBuffer.h"
#include "luth/renderer/Texture.h"

#include <entt/entt.hpp>
#include <unordered_map>

namespace Luth
{
    struct GlobalUniforms {
        glm::mat4 viewProjection;
        glm::mat4 view;
        glm::mat4 projection;
        glm::vec3 cameraPos;
        float time;
    };

    class RenderingSystem : public System
    {
    public:
        RenderingSystem(u32 viewportWidth = 1280, u32 viewportHeight = 720);
        ~RenderingSystem();

        void Update(entt::registry& registry) override;
        void Resize(u32 width, u32 height);

        std::shared_ptr<Texture> GetSceneColor() const { return m_SceneColor; }
        
        u64 GetFrameAllocatorUsage() const { return m_FrameAllocator->GetUsedMemory(); }
        u64 GetFrameAllocatorTotal() const { return m_FrameAllocator->GetTotalSize(); }

    private:
        void InitGlobalUniforms();
        void UpdateGlobalUniforms();

        // Memory
        std::unique_ptr<LinearAllocator> m_FrameAllocator;

        // Resources
        std::shared_ptr<Texture> m_SceneColor;
        
        std::shared_ptr<VKUniformBuffer> m_GlobalUniformBuffer;
        VkDescriptorSetLayout m_GlobalSetLayout = VK_NULL_HANDLE;
        VkDescriptorSet m_GlobalDescriptorSet = VK_NULL_HANDLE;

        // Vulkan Test Pipeline
        std::unique_ptr<VKPipeline> m_TrianglePipeline;
    };

}
