#pragma once

#include "luth/scene/System.h"
#include "luth/memory/Memory.h"
#include "luth/renderer/rendergraph/RenderGraph.h"
#include "luth/renderer/backend/vulkan/VulkanPipeline.h"
#include "luth/renderer/backend/vulkan/VulkanBuffer.h"
#include "luth/renderer/Texture.h"
#include "luth/renderer/Material.h"
#include "luth/core/UUID.h"

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

    struct ObjectPushConstants {
        glm::mat4 modelMatrix;  // 64 bytes
        u32 materialIndex;      // 4 bytes — index into material SSBO
        u32 _pad[3];            // 12 bytes padding (80 total)
    };

    class RenderingSystem : public System
    {
    public:
        RenderingSystem(u32 viewportWidth = 1280, u32 viewportHeight = 720);
        ~RenderingSystem();

        void Update(Scene* scene) override;
        void Resize(u32 width, u32 height);

        std::shared_ptr<Texture> GetSceneColor() const { return m_SceneColor; }

        u64 GetFrameAllocatorUsage() const { return m_FrameAllocator->GetUsedMemory(); }
        u64 GetFrameAllocatorTotal() const { return m_FrameAllocator->GetTotalSize(); }

    private:
        void InitGlobalUniforms();
        void UpdateGlobalUniforms();
        void CreatePipelines();
        u32  EnsureMaterialRegistered(Material* material);

        RG::ResourceHandle AddGeometryPass(RG::RenderGraph& rg, entt::registry& registry);
        void AddImGuiPass(RG::RenderGraph& rg, RG::ResourceHandle sceneColor);

        // Memory
        std::unique_ptr<Memory::LinearAllocator> m_FrameAllocator;

        // Resources
        std::shared_ptr<Texture> m_SceneColor;

        std::shared_ptr<VKUniformBuffer> m_GlobalUniformBuffer;
        VkDescriptorSetLayout m_GlobalSetLayout = VK_NULL_HANDLE;
        VkDescriptorSet m_GlobalDescriptorSet = VK_NULL_HANDLE;

        // PBR Pipelines (one per RenderMode)
        std::unordered_map<Material::RenderMode, std::unique_ptr<VKPipeline>> m_Pipelines;
        std::vector<u32> m_PBRVertSpv;
        std::vector<u32> m_PBRFragSpv;

        // Material SSBO slot tracking (MaterialUUID -> SSBO index)
        std::unordered_map<UUID, u32, UUIDHash> m_MaterialSlotMap;
    };

}
