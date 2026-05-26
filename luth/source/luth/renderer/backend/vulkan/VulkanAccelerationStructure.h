#pragma once

#include "luth/core/types/LuthTypes.h"
#include "VulkanAllocator.h"

#include <memory>
#include <vulkan/vulkan.h>

namespace Luth
{
    class Mesh;

    // RAII wrapper for a single VkAccelerationStructureKHR + its persistent backing VkBuffer.
    // Used for both BLAS (per-mesh, owned by Mesh) and TLAS (per-frame, owned by RtSubsystem).
    // Dtor pushes both handle destruction + buffer free into VulkanContext::PushDeletion so
    // they retire N+2 frames out — safe against any in-flight cmd buffer that still references
    // the AS handle in a build / traceRays call.
    class VKAccelerationStructure
    {
    public:
        VKAccelerationStructure() = default;
        ~VKAccelerationStructure();

        VKAccelerationStructure(const VKAccelerationStructure&) = delete;
        VKAccelerationStructure& operator=(const VKAccelerationStructure&) = delete;

        VkAccelerationStructureKHR GetHandle()        const { return m_Handle; }
        VkDeviceAddress            GetDeviceAddress() const { return m_DeviceAddress; }

        // Per-mesh static BLAS factory. Synchronous main-thread ImmediateSubmit on the graphics
        // queue (graphics families always advertise VK_QUEUE_COMPUTE_BIT per spec, which is what
        // vkCmdBuildAccelerationStructuresKHR requires). PREFER_FAST_TRACE flag per NVIDIA RTX
        // best practices — static BLAS optimizes for ray-trace performance, build cost is paid once.
        // Gates on UploadContext::WaitForUpload of the VB/IB upload fences before recording the
        // build — VB/IB upload runs on a separate submission chain (transfer queue), so the
        // graphics-queue draws' implicit serialize does NOT cover this build path.
        static std::shared_ptr<VKAccelerationStructure> CreateStaticBLAS(const Mesh& mesh);

    private:
        VkAccelerationStructureKHR m_Handle         = VK_NULL_HANDLE;
        VkBuffer                   m_StorageBuffer  = VK_NULL_HANDLE;
        VmaAllocation              m_StorageAlloc   = nullptr;
        VkDeviceAddress            m_DeviceAddress  = 0;
    };
}
