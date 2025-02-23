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

        // Temporary for debugging
        VkInstance GetInstance() const { return m_Instance; }

    private:
        void CreateInstance();
        void SetupDebugMessenger();
        void DestroyDebugMessenger();
        bool CheckValidationLayerSupport() const;

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
