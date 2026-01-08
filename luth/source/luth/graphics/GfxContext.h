#pragma once

#include "luth/core/LuthTypes.h"
#include <vulkan/vulkan.h>
#include <vector>
#include <optional>

namespace Luth::Gfx
{
    struct GfxQueue
    {
        VkQueue handle = VK_NULL_HANDLE;
        u32 familyIndex = 0;
    };

    class GfxContext
    {
    public:
        static void Init(void* windowHandle);
        static void Shutdown();
        static GfxContext& Get();

        VkInstance GetInstance() const { return m_Instance; }
        VkDevice GetDevice() const { return m_Device; }
        VkPhysicalDevice GetPhysicalDevice() const { return m_PhysicalDevice; }
        VkSurfaceKHR GetSurface() const { return m_Surface; }
        
        const GfxQueue& GetGraphicsQueue() const { return m_GraphicsQueue; }
        const GfxQueue& GetPresentQueue() const { return m_PresentQueue; }
        const GfxQueue& GetComputeQueue() const { return m_ComputeQueue; }
        const GfxQueue& GetTransferQueue() const { return m_TransferQueue; }

        // Helper to find memory type
        u32 FindMemoryType(u32 typeFilter, VkMemoryPropertyFlags properties) const;

    private:
        GfxContext() = default;
        ~GfxContext() = default;

        void CreateInstance();
        void CreateSurface(void* windowHandle);
        void SelectPhysicalDevice();
        void CreateLogicalDevice();

        // Debug
        void SetupDebugMessenger();
        
        VkInstance m_Instance = VK_NULL_HANDLE;
        VkDebugUtilsMessengerEXT m_DebugMessenger = VK_NULL_HANDLE;
        VkSurfaceKHR m_Surface = VK_NULL_HANDLE;
        
        VkPhysicalDevice m_PhysicalDevice = VK_NULL_HANDLE;
        VkDevice m_Device = VK_NULL_HANDLE;

        GfxQueue m_GraphicsQueue;
        GfxQueue m_PresentQueue;
        GfxQueue m_ComputeQueue;
        GfxQueue m_TransferQueue;

        static GfxContext* s_Instance;
    };
}
