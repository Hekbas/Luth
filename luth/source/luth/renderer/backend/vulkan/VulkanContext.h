#pragma once

#include "luth/core/types/LuthTypes.h"
#include "luth/core/FrameData.h"
#include "luth/jobs/SpinLock.h"
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
    // Singleton context holding the VkInstance, VkDevice, queues, VMA allocator, surface, the
    // global bindless descriptor set, the RenderGraph resource cache, and the deletion queues
    // that subsystems push retired GPU resources into. Created by VulkanBackend::Init; shared
    // across every backend module that needs raw Vulkan handles.
    class VulkanContext
    {
    public:
        static void Init(void* windowHandle);
        static void Shutdown();
        static VulkanContext& Get();

        // Device-level KHR ray-tracing entry points loaded by LoadRayTracingFunctions.
        // Populated at CreateLogicalDevice time; any null pointer is a fatal CRITICAL log
        // because RT-mandatory means missing fps are not a soft-fail.
        struct RtFunctions
        {
            PFN_vkCreateAccelerationStructureKHR        vkCreateAccelerationStructureKHR        = nullptr;
            PFN_vkDestroyAccelerationStructureKHR       vkDestroyAccelerationStructureKHR       = nullptr;
            PFN_vkGetAccelerationStructureBuildSizesKHR vkGetAccelerationStructureBuildSizesKHR = nullptr;
            PFN_vkCmdBuildAccelerationStructuresKHR     vkCmdBuildAccelerationStructuresKHR     = nullptr;
            PFN_vkGetAccelerationStructureDeviceAddressKHR vkGetAccelerationStructureDeviceAddressKHR = nullptr;
            PFN_vkCreateRayTracingPipelinesKHR          vkCreateRayTracingPipelinesKHR          = nullptr;
            PFN_vkGetRayTracingShaderGroupHandlesKHR    vkGetRayTracingShaderGroupHandlesKHR    = nullptr;
            PFN_vkCmdTraceRaysKHR                       vkCmdTraceRaysKHR                       = nullptr;
        };

        // VK_NV_device_diagnostic_checkpoints — optional NV-only diagnostic. Enabled when the
        // physical device advertises the extension; otherwise both pointers stay null and call
        // sites no-op. Used to localize the failing GPU command after VK_ERROR_DEVICE_LOST.
        struct CheckpointFunctions
        {
            PFN_vkCmdSetCheckpointNV         vkCmdSetCheckpointNV         = nullptr;
            PFN_vkGetQueueCheckpointDataNV   vkGetQueueCheckpointDataNV   = nullptr;
        };

        VkInstance GetInstance() const { return m_Instance; }
        VkPhysicalDevice GetPhysicalDevice() const { return m_PhysicalDevice; }
        VkDevice GetDevice() const { return m_Device; }
        VmaAllocator GetAllocator() const { return m_Allocator; }
        const VkPhysicalDeviceProperties& GetPhysicalDeviceProperties() const { return m_PhysicalDeviceProperties; }
        const RtFunctions& GetRtFn() const { return m_RtFn; }
        const CheckpointFunctions& GetCheckpointFn() const { return m_CheckpointFn; }
        bool HasCheckpoints() const { return m_CheckpointFn.vkCmdSetCheckpointNV != nullptr; }
        const VkPhysicalDeviceRayTracingPipelinePropertiesKHR&    GetRtPipelineProperties() const { return m_RtPipelineProperties; }
        const VkPhysicalDeviceAccelerationStructurePropertiesKHR& GetAsProperties()         const { return m_AsProperties; }
        // UBO descriptor base offsets (and size) must satisfy this when sub-allocating from a tagged page.
        u64 GetMinUniformBufferAlignment() const { return m_PhysicalDeviceProperties.limits.minUniformBufferOffsetAlignment; }
        BindlessDescriptorSet& GetBindlessSet() { return m_BindlessSet; }
        RG::RenderResourceCache& GetResourceCache() { return m_ResourceCache; } // Getter

        // Queue Access — graphics is always present; compute/transfer alias to graphics on single-family GPUs.
        VkQueue GetGraphicsQueue() const { return m_GraphicsQueue; }
        VkQueue GetComputeQueue()  const { return m_ComputeQueue;  }
        VkQueue GetTransferQueue() const { return m_TransferQueue; }
        u32 GetGraphicsFamily() const { return m_GraphicsFamily; }
        u32 GetComputeFamily()  const { return m_ComputeFamily;  }
        u32 GetTransferFamily() const { return m_TransferFamily; }
        bool IsAsyncCompute()   const { return m_ComputeIsAsync;  }
        bool IsAsyncTransfer()  const { return m_TransferIsAsync; }

        // Deduped {graphics, compute, transfer} family list for VK_SHARING_MODE_CONCURRENT resource creation.
        // Single-family layouts collapse to size 1 — callers should fall back to EXCLUSIVE in that case.
        const std::vector<u32>& GetConcurrentFamilyIndices() const { return m_ConcurrentFamilyIndices; }

        // Apply CONCURRENT sharing across all in-use queue families. When the deduped set is size 1, leaves the
        // create-info at the caller's default (typically EXCLUSIVE) — CONCURRENT with one family is spec UB.
        // See docs/development/arch/multi-queue.md for the per-resource opt-in policy.
        void ApplyConcurrentSharing(VkBufferCreateInfo& info) const;
        void ApplyConcurrentSharing(VkImageCreateInfo&  info) const;

        // Helper to find memory types (if not using VMA for some reason)
        u32 FindMemoryType(u32 typeFilter, VkMemoryPropertyFlags properties);

        // Submit a command immediately and wait for it to finish (used for resource uploads)
        void ImmediateSubmit(std::function<void(VkCommandBuffer)>&& function);

        // Thread-safe queue submission. Per-queue mutex guards the kernel syscall (vkQueueSubmit*) — this is the
        // documented case where std::mutex is correct on a hot path per arch/memory.md (exceeds SpinLock contract).
        bool Submit(const VkSubmitInfo& submitInfo, VkFence fence);
        bool Submit2(const VkSubmitInfo2& submitInfo, VkFence fence);          // Alias of SubmitGraphics2 — back-compat.
        bool SubmitGraphics2(const VkSubmitInfo2& submitInfo, VkFence fence);
        bool SubmitCompute2 (const VkSubmitInfo2& submitInfo, VkFence fence);
        bool SubmitTransfer2(const VkSubmitInfo2& submitInfo, VkFence fence);
        VkResult Present(const VkPresentInfoKHR& presentInfo);

        // Safe Resource Deletion
        void PushDeletion(std::function<void()>&& function);
        void FlushDeletionQueue();
        void FlushAllDeletionQueues();

        // On VK_ERROR_DEVICE_LOST from any submit: dump the last checkpoint per queue to the log.
        // Idempotent — only fires once per process lifetime; subsequent calls are no-ops.
        // No-op when VK_NV_device_diagnostic_checkpoints isn't enabled on this device.
        void DumpCheckpointsOnDeviceLost(const char* originLabel);

        // Called by RendererAPI
        void SetCurrentFrameIndex(u32 index) { m_CurrentFrameIndex = index; }

        // VK_EXT_debug_utils tag — validation layer prints `name` alongside the
        // raw handle in error messages. No-op when validation/debug-utils is off.
        static void SetDebugName(VkDescriptorSet set, const char* name);

    private:
        void CreateInstance();
        void SetupDebugMessenger();
        void PickPhysicalDevice();
        void CreateLogicalDevice();
        void LoadRayTracingFunctions();
        void LoadCheckpointFunctions();
        void InitAllocator();

        // Validation layers gated by LUTH_ENABLE_VALIDATION (luth/core/BuildConfig.h).
        // Default: on in Debug, off in Release/Dist. Override per-config or via the LUTH_VALIDATION env.
        bool CheckValidationLayerSupport();
        // Resolve LUTH_VALIDATION → m_EnableValidationLayers + m_ValTiers (any-build runtime opt-in).
        // see arch/gpu-crash-debugging.md
        void ResolveValidationConfig();
        std::vector<const char*> m_ValidationLayers = { "VK_LAYER_KHRONOS_validation" };
        bool m_EnableValidationLayers = (LUTH_ENABLE_VALIDATION != 0);

        // Feature tiers selected by LUTH_VALIDATION; core is always implied when validation is on.
        struct ValidationTiers { bool sync=false, gpuav=false, bestPractices=false, rtValidation=false, uncapped=false; };
        ValidationTiers m_ValTiers{};

        VkInstance m_Instance = VK_NULL_HANDLE;
        VkDebugUtilsMessengerEXT m_DebugMessenger = VK_NULL_HANDLE;
        VkPhysicalDevice m_PhysicalDevice = VK_NULL_HANDLE;
        VkPhysicalDeviceProperties m_PhysicalDeviceProperties;
        VkPhysicalDeviceRayTracingPipelinePropertiesKHR    m_RtPipelineProperties{};
        VkPhysicalDeviceAccelerationStructurePropertiesKHR m_AsProperties{};
        VkDevice m_Device = VK_NULL_HANDLE;
        RtFunctions m_RtFn{};
        CheckpointFunctions m_CheckpointFn{};
        bool m_CheckpointsAvailable = false;
        
        // Queue handles. Compute/transfer alias to graphics when no distinct family exists — callers route through
        // SubmitCompute2/SubmitTransfer2 regardless, so the alias is invisible at the call site. Each queue has its
        // own mutex (vkQueueSubmit2 is a kernel syscall — std::mutex is the right primitive here, see arch/memory.md).
        VkQueue m_GraphicsQueue = VK_NULL_HANDLE;
        VkQueue m_ComputeQueue  = VK_NULL_HANDLE;
        VkQueue m_TransferQueue = VK_NULL_HANDLE;
        std::mutex m_QueueMutex;
        std::mutex m_ComputeQueueMutex;
        std::mutex m_TransferQueueMutex;
        u32 m_GraphicsFamily = (u32)-1;
        u32 m_ComputeFamily  = (u32)-1;
        u32 m_TransferFamily = (u32)-1;
        bool m_ComputeIsAsync  = false;  // true iff compute family distinct from graphics
        bool m_TransferIsAsync = false;  // true iff transfer family distinct from graphics
        std::vector<u32> m_ConcurrentFamilyIndices;  // Deduped list — used for CONCURRENT sharing-mode resources.
        std::mutex m_CommandPoolMutex;
        VkCommandPool m_CommandPool = VK_NULL_HANDLE;
        BindlessDescriptorSet m_BindlessSet;
        RG::RenderResourceCache m_ResourceCache; // Instance

        VmaAllocator m_Allocator = VK_NULL_HANDLE;
        void* m_WindowHandle = nullptr; // Raw GLFW window handle

        // Per-frame ring; resource dtors push from any thread (V1 SpinLock — push/swap stays under <100 cycles).
        struct DeletionQueue { std::deque<std::function<void()>> deletors; };
        DeletionQueue m_DeletionQueues[MAX_FRAMES_IN_FLIGHT];
        SpinLock m_DeletionLock;
        u32 m_CurrentFrameIndex = 0;
    };
}
