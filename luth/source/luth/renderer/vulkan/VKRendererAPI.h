#pragma once

#include "luth/core/Log.h"
#include "luth/window/Window.h"
#include "luth/renderer/Renderer.h"
#include "luth/renderer/vulkan/VKDevice.h"
#include "luth/renderer/vulkan/VKSwapchain.h"
#include "luth/renderer/vulkan/VKRenderPass.h"
#include "luth/renderer/vulkan/VKGraphicsPipeline.h"

#include <vulkan/vulkan.h>
#include <vector>
#include <memory>

namespace Luth
{
    class VKRendererAPI : public RendererAPI
    {
    public:
        virtual void Init() override;
        virtual void Shutdown() override;

        virtual void SetViewport(u32 x, u32 y, u32 width, u32 height) override;
        virtual void SetClearColor(const glm::vec4& color) override;
        virtual void Clear() override;

        //void EnableDepthTest(bool enable) override;
        //bool IsDepthTestEnabled() const override { return true; }
        //
        //void EnableBlending(bool enable) override;
        //void SetBlendFunction(u32 srcFactor, u32 dstFactor) override;

        virtual void DrawIndexed(u32 count) override;

        // Temporary for debugging
        VkInstance GetInstance() const { return m_Instance; }

    private:
        void CreateInstance();
        void CreateSurface();
        void CreateDevice();
        void CreateSwapChain();
        void CreateRenderPass();
        void CreateGraphicsPipeline();

        void SetupDebugMessenger();
        void DestroyDebugMessenger();
        bool CheckValidationLayerSupport() const;
        void PrintExtensions() const;
        void PrintLayers() const;

        // Core Vulkan objects
        VkInstance m_Instance = VK_NULL_HANDLE;
        VkSurfaceKHR m_Surface = VK_NULL_HANDLE;
        VkDebugUtilsMessengerEXT m_DebugMessenger = VK_NULL_HANDLE;

        std::unique_ptr<VKPhysicalDevice> m_PhysicalDevice;
        std::unique_ptr<VKLogicalDevice> m_LogicalDevice;
        std::unique_ptr<VKSwapchain> m_Swapchain;
        std::unique_ptr<VKRenderPass> m_RenderPass;
        std::unique_ptr<VKGraphicsPipeline> m_GraphicsPipeline;

        // Configuration
        const std::vector<const char*> m_ValidationLayers = {
            "VK_LAYER_KHRONOS_validation"
        };
        const std::vector<const char*> m_InstanceExtensions = {
            VK_EXT_DEBUG_UTILS_EXTENSION_NAME
        };

        #ifdef NDEBUG
            const bool m_EnableValidationLayers = false;
        #else
            const bool m_EnableValidationLayers = true;
        #endif
    };
}
