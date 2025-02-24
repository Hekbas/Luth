#pragma once

#include "luth/renderer/Renderer.h"
#include "luth/core/Log.h"

#include <vulkan/vulkan.h>
#include <vector>

namespace Luth
{
    class VKRenderer final : public Renderer
    {
    public:
        VKRenderer();
        virtual ~VKRenderer();

        void Init() override;
        void Shutdown() override;

        void SetClearColor(const glm::vec4& color) override;
        void Clear() override;
        void SetViewport(u32 x, u32 y, u32 width, u32 height) override;

        void EnableDepthTest(bool enable) override;
        bool IsDepthTestEnabled() const override { return true; }

        void EnableBlending(bool enable) override;
        void SetBlendFunction(u32 srcFactor, u32 dstFactor) override;

        void DrawIndexed(u32 count) override;

        // Temporary for debugging
        VkInstance GetInstance() const { return m_Instance; }

    private:
        void CreateInstance();
        void SetupDebugMessenger();
        void DestroyDebugMessenger();
        bool CheckValidationLayerSupport() const;
        void PrintExtensions() const;
        void PrintLayers() const;

        // Core Vulkan objects
        VkInstance m_Instance = VK_NULL_HANDLE;
        VkDebugUtilsMessengerEXT m_DebugMessenger = VK_NULL_HANDLE;

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
