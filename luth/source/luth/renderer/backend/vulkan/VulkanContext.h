#pragma once

#include "luth/core/types/LuthTypes.h"
#include "luth/core/FrameData.h"
#include <vulkan/vulkan.h>
#include <vector>
#include <functional>
#include <deque>
#include <mutex>
#include "luth/renderer/backend/vulkan/VulkanDescriptors.h"
#include "luth/renderer/rendergraph/RenderResourceCache.h"

// Forward declare VMA types to avoid including the huge header here
typedef struct VmaAllocator_T* VmaAllocator;

namespace Luth
{
    class VulkanContext
    {
    public:
        static void Init(void* windowHandle);
        static void Shutdown();
        static VulkanContext& Get();

        VkInstance GetInstance() const { return m_Instance; }
        VkPhysicalDevice GetPhysicalDevice() const { return m_PhysicalDevice; }
        VkDevice GetDevice() const { return m_Device; }
        VmaAllocator GetAllocator() const { return m_Allocator; }
        const VkPhysicalDeviceProperties& GetPhysicalDeviceProperties() const { return m_PhysicalDeviceProperties; }
        BindlessDescriptorSet& GetBindlessSet() { return m_BindlessSet; }
        DescriptorAllocator& GetDescriptorAllocator() { return m_DescriptorAllocator; }
        RG::RenderResourceCache& GetResourceCache() { return m_ResourceCache; } // Getter

        // Queue Access
        VkQueue GetGraphicsQueue() const { return m_GraphicsQueue; }
        u32 GetGraphicsFamily() const { return m_GraphicsFamily; }

        // Helper to find memory types (if not using VMA for some reason)
        u32 FindMemoryType(u32 typeFilter, VkMemoryPropertyFlags properties);

        // Submit a command immediately and wait for it to finish (used for resource uploads)
        void ImmediateSubmit(std::function<void(VkCommandBuffer)>&& function);

        // Thread-safe queue submission
        bool Submit(const VkSubmitInfo& submitInfo, VkFence fence);
        bool Submit2(const VkSubmitInfo2& submitInfo, VkFence fence);
        VkResult Present(const VkPresentInfoKHR& presentInfo);

        // Safe Resource Deletion
        void PushDeletion(std::function<void()>&& function);
        void FlushDeletionQueue();
        void FlushAllDeletionQueues();
        
        // Called by RendererAPI
        void SetCurrentFrameIndex(u32 index) { m_CurrentFrameIndex = index; }

    private:
        void CreateInstance();
        void SetupDebugMessenger();
        void PickPhysicalDevice();
        void CreateLogicalDevice();
        void InitAllocator();

        // Validation layers gated by LUTH_ENABLE_VALIDATION (luth/core/BuildConfig.h).
        // Default: on in Debug, off in Release/Dist. Override per-config in premake.
        bool CheckValidationLayerSupport();
        std::vector<const char*> m_ValidationLayers = { "VK_LAYER_KHRONOS_validation" };
        bool m_EnableValidationLayers = (LUTH_ENABLE_VALIDATION != 0);

        VkInstance m_Instance = VK_NULL_HANDLE;
        VkDebugUtilsMessengerEXT m_DebugMessenger = VK_NULL_HANDLE;
        VkPhysicalDevice m_PhysicalDevice = VK_NULL_HANDLE;
        VkPhysicalDeviceProperties m_PhysicalDeviceProperties;
        VkDevice m_Device = VK_NULL_HANDLE;
        
        VkQueue m_GraphicsQueue = VK_NULL_HANDLE;
        std::mutex m_QueueMutex;
        u32 m_GraphicsFamily = -1;
        std::mutex m_CommandPoolMutex;
        VkCommandPool m_CommandPool = VK_NULL_HANDLE;
        BindlessDescriptorSet m_BindlessSet;
        DescriptorAllocator m_DescriptorAllocator;
        RG::RenderResourceCache m_ResourceCache; // Instance

        VmaAllocator m_Allocator = VK_NULL_HANDLE;
        void* m_WindowHandle = nullptr; // Raw GLFW window handle

        // Resource Deletion Queue
        struct DeletionQueue
        {
            std::deque<std::function<void()>> deletors;
        };
        // Uses global MAX_FRAMES_IN_FLIGHT from FrameData.h. Resource dtors
        // (VKTexture/VKBuffer/etc.) push from any thread that holds the last
        // shared_ptr — guard the deque with m_DeletionMutex.
        DeletionQueue m_DeletionQueues[MAX_FRAMES_IN_FLIGHT];
        std::mutex m_DeletionMutex;
        u32 m_CurrentFrameIndex = 0;
    };
}
