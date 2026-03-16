#pragma once

#include "luth/scene/System.h"
#include "luth/memory/Memory.h"
#include "luth/renderer/rendergraph/RenderGraph.h"
#include "luth/renderer/backend/vulkan/VulkanPipeline.h"
#include "luth/renderer/backend/vulkan/VulkanBuffer.h"
#include "luth/renderer/Texture.h"
#include "luth/renderer/Material.h"
#include "luth/core/UUID.h"
#include "luth/resources/FileWatcher.h"

#include <entt/entt.hpp>
#include <unordered_map>
#include <set>
#include <mutex>

namespace Luth
{
    struct GlobalUniforms {
        glm::mat4 viewProjection;
        glm::mat4 view;
        glm::mat4 projection;
        glm::vec3 cameraPos;
        float     time;
        glm::mat4 lightSpaceMatrix;  // Set by UpdateLightUniforms before upload
        float     shadowBias;
        float     _pad[3];
    };

    struct ObjectPushConstants {
        glm::mat4 modelMatrix;  // 64 bytes
        u32 materialIndex;      // 4 bytes — index into material SSBO
        u32 _pad[3];            // 12 bytes padding (80 total)
    };

    // ---- Light data structs (mirrored in pbr.frag Set 3) ----

    struct DirectionalLightData {
        glm::vec3 direction;   // 12
        float     intensity;   // 4
        glm::vec3 color;       // 12
        float     _pad;        // 4
    };  // 32 bytes

    struct PointLightData {
        glm::vec3 position;    // 12
        float     range;       // 4
        glm::vec3 color;       // 12
        float     intensity;   // 4
    };  // 32 bytes

    struct LightUniforms {
        DirectionalLightData dirLight;
        PointLightData       pointLights[64];
        int                  numPointLights;
        int                  _pad[3];
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
        void InitShadowResources();
        void UpdateGlobalUniforms();
        void UpdateLightUniforms(Scene* scene);
        void CreatePipelines();
        u32  EnsureMaterialRegistered(Material* material);

        RG::ResourceHandle AddShadowPass(RG::RenderGraph& rg, entt::registry& registry);
        RG::ResourceHandle AddGeometryPass(RG::RenderGraph& rg, entt::registry& registry, RG::ResourceHandle shadowMapHandle);
        void AddImGuiPass(RG::RenderGraph& rg, RG::ResourceHandle sceneColor);

        // Memory
        std::unique_ptr<Memory::LinearAllocator> m_FrameAllocator;

        // Scene color output
        std::shared_ptr<Texture> m_SceneColor;

        // Global UBO (Set 0)
        std::shared_ptr<VKUniformBuffer> m_GlobalUniformBuffer;
        VkDescriptorSetLayout m_GlobalSetLayout = VK_NULL_HANDLE;
        VkDescriptorSet m_GlobalDescriptorSet = VK_NULL_HANDLE;

        // Light space matrix (computed in UpdateLightUniforms, uploaded in UpdateGlobalUniforms)
        glm::mat4 m_CachedLightSpaceMatrix = glm::mat4(1.0f);
        float     m_CachedShadowBias = 0.005f;
        bool      m_CachedCastShadows = true;

        // Shadow map
        std::shared_ptr<Texture> m_ShadowMap;
        VkSampler                m_ShadowSampler = VK_NULL_HANDLE;

        // Light UBO + shadow descriptor (Set 3)
        std::shared_ptr<VKUniformBuffer> m_LightUniformBuffer;
        VkDescriptorPool      m_LightDescPool   = VK_NULL_HANDLE;
        VkDescriptorSetLayout m_LightSetLayout  = VK_NULL_HANDLE;
        VkDescriptorSet       m_LightDescSet    = VK_NULL_HANDLE;

        // Shadow pipeline (depth-only)
        std::unique_ptr<VKPipeline> m_ShadowPipeline;
        std::vector<u32>            m_ShadowVertSpv;
        std::vector<u32>            m_ShadowFragSpv;

        // PBR Pipelines (one per RenderMode)
        std::unordered_map<Material::RenderMode, std::unique_ptr<VKPipeline>> m_Pipelines;
        std::vector<u32> m_PBRVertSpv;
        std::vector<u32> m_PBRFragSpv;

        // Material SSBO slot tracking (MaterialUUID -> SSBO index)
        std::unordered_map<UUID, u32, UUIDHash> m_MaterialSlotMap;

        // Shader hot-reload
        FileWatcher m_ShaderWatcher;
        std::mutex m_ReloadMutex;
        std::set<std::string> m_PendingReloads;
    };

}
