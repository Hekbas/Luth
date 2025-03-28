#pragma once

#include "luth/core/Log.h"
#include "luth/window/Window.h"
#include "luth/renderer/Renderer.h"
#include "luth/renderer/vulkan/VKDevice.h"
#include "luth/renderer/vulkan/VKSwapchain.h"
#include "luth/renderer/vulkan/VKRenderPass.h"
#include "luth/renderer/vulkan/VKGraphicsPipeline.h"
#include "luth/renderer/vulkan/VKFramebuffer.h"
#include "luth/renderer/vulkan/VKCommandPool.h"
#include "luth/renderer/vulkan/VKSync.h"
#include "luth/renderer/vulkan/VKMesh.h"

#include <vulkan/vulkan.h>
#include <vector>
#include <memory>

namespace Luth
{
    class VKRendererAPI : public RendererAPI
    {
    public:
        struct UniformBufferObject {
            glm::mat4 model;
            glm::mat4 view;
            glm::mat4 proj;
        };

        virtual void Init() override;
        virtual void Shutdown() override;

        virtual void BindFramebuffer(const std::shared_ptr<Framebuffer>& framebuffer) override;

        virtual void SetViewport(u32 x, u32 y, u32 width, u32 height) override;
        virtual void SetClearColor(const glm::vec4& color) override;
        virtual void Clear() override;

        //void EnableDepthTest(bool enable) override;
        //bool IsDepthTestEnabled() const override { return true; }
        //
        //void EnableBlending(bool enable) override;
        //void SetBlendFunction(u32 srcFactor, u32 dstFactor) override;

        virtual void SubmitMesh(const std::shared_ptr<Mesh>& mesh) override;

        virtual void DrawIndexed(u32 count) override;
        virtual void DrawFrame() override;

        VkInstance GetInstance() const { return m_Instance; }
        VkSurfaceKHR GetSurface() const { return m_Surface; }

        const VKLogicalDevice& GetLogicalDevice() const { return *m_LogicalDevice; }
        const VKPhysicalDevice& GetPhysicalDevice() const { return *m_PhysicalDevice; }
        const VKSwapchain& GetSwapchain() const { return *m_Swapchain; }
        const VKCommandPool& GetCommandPool() const { return *m_CommandPool; }

    private:
        // Core Vulkan components
        void CreateInstance();
        void CreateSurface();
        void CreateDevice();
        void CreateSwapchain();
        void CreateRenderPass();
        void CreateDescriptorSetLayout();
        void CreateGraphicsPipeline();
        void CreateFramebuffers();
        void CreateCommandPool();
        void CreateUniformBuffers();
        void CreateDescriptorPool();
        void AllocateDescriptorSets();
        void UpdateDescriptorSets();
        void CreateCommandBuffers();
        void CreateSyncObjects();
        void RecreateSwapchain();

        // Command recording
        void RecordCommandBuffer(VkCommandBuffer commandBuffer, uint32_t imageIndex);

        // Validation layers
        void SetupDebugMessenger();
        void DestroyDebugMessenger();
        bool CheckValidationLayerSupport() const;
        void PrintExtensions() const;
        void PrintLayers() const;

        // Vulkan objects
        VkInstance m_Instance = VK_NULL_HANDLE;
        VkSurfaceKHR m_Surface = VK_NULL_HANDLE;
        VkDebugUtilsMessengerEXT m_DebugMessenger = VK_NULL_HANDLE;

        // Managed resources
        std::unique_ptr<VKPhysicalDevice> m_PhysicalDevice;
        std::unique_ptr<VKLogicalDevice> m_LogicalDevice;
        std::unique_ptr<VKSwapchain> m_Swapchain;
        std::unique_ptr<VKRenderPass> m_RenderPass;
        std::unique_ptr<VKGraphicsPipeline> m_GraphicsPipeline;
        std::vector<std::unique_ptr<VKFramebuffer>> m_Framebuffers;
        std::unique_ptr<VKCommandPool> m_CommandPool;
        std::vector<VkCommandBuffer> m_CommandBuffers;
        std::unique_ptr<VKSync> m_Sync;
        std::shared_ptr<VKMesh> m_CurrentMesh;

        // Uniform Resources
        VkDescriptorSetLayout m_DescriptorSetLayout;
        VkDescriptorPool m_DescriptorPool;
        std::vector<VkBuffer> m_UniformBuffers;
        std::vector<VkDeviceMemory> m_UniformBuffersMemory;
        std::vector<VkDescriptorSet> m_DescriptorSets;

        // Configuration
        const std::vector<const char*> m_ValidationLayers = { "VK_LAYER_KHRONOS_validation" };
        const std::vector<const char*> m_InstanceExtensions = { VK_EXT_DEBUG_UTILS_EXTENSION_NAME };
        static constexpr size_t MAX_FRAMES_IN_FLIGHT = 2;

        #ifdef NDEBUG
            const bool m_EnableValidationLayers = false;
        #else
            const bool m_EnableValidationLayers = true;
        #endif
    };
}
