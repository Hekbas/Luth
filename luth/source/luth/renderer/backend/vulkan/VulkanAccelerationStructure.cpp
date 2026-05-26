#include "luthpch.h"
#include "VulkanAccelerationStructure.h"
#include "VulkanContext.h"
#include "VulkanBuffer.h"
#include "UploadContext.h"
#include "luth/renderer/resources/Mesh.h"
#include "luth/renderer/resources/Model.h"
#include "luth/core/diagnostics/Log.h"

#include <vma/vk_mem_alloc.h>
#include <algorithm>

namespace Luth
{
    namespace
    {
        constexpr u64 AlignUp(u64 value, u64 alignment)
        {
            return (value + (alignment - 1)) & ~(alignment - 1);
        }
    }

    VKAccelerationStructure::~VKAccelerationStructure()
    {
        if (m_Handle == VK_NULL_HANDLE) return;
        auto handle = m_Handle;
        auto buffer = m_StorageBuffer;
        auto alloc  = m_StorageAlloc;
        VulkanContext::Get().PushDeletion([handle, buffer, alloc]() {
            auto& ctx = VulkanContext::Get();
            ctx.GetRtFn().vkDestroyAccelerationStructureKHR(ctx.GetDevice(), handle, nullptr);
            VulkanAllocator::FreeBuffer(buffer, alloc);
        });
    }

    std::shared_ptr<VKAccelerationStructure> VKAccelerationStructure::CreateStaticBLAS(const Mesh& mesh)
    {
        auto& ctx = VulkanContext::Get();
        VkDevice device = ctx.GetDevice();
        const auto& rt  = ctx.GetRtFn();

        auto vb = std::dynamic_pointer_cast<VKVertexBuffer>(mesh.GetVertexBuffer());
        auto ib = std::dynamic_pointer_cast<VKIndexBuffer>(mesh.GetIndexBuffer());
        if (!vb || !ib)
        {
            LH_CORE_ERROR("CreateStaticBLAS: non-Vulkan VB/IB on mesh — skipping BLAS");
            return nullptr;
        }
        const u32 vertCount  = mesh.GetVertexCount();
        const u32 indexCount = ib->GetCount();
        if (vertCount == 0 || indexCount == 0) return nullptr;

        // VB/IB upload runs on the transfer-queue submission chain via UploadContext; the build
        // we're about to record reads them on the graphics queue, which is a separate submission
        // and thus not serialized against the upload. Wait explicitly on the upload fence.
        const u64 fence = std::max(vb->GetUploadFence(), ib->GetUploadFence());
        if (fence > 0) UploadContext::Get().WaitForUpload(fence);

        // Position is at offset 0 in both Vertex and SkinnedVertex. Stride keeps the rest of the
        // vertex fields skipped naturally — AS-build only reads positions per the geometry desc.
        VkAccelerationStructureGeometryTrianglesDataKHR tri{
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR };
        tri.vertexFormat                = VK_FORMAT_R32G32B32_SFLOAT;
        tri.vertexData.deviceAddress    = vb->GetDeviceAddress();
        tri.vertexStride                = mesh.IsSkinned() ? sizeof(SkinnedVertex) : sizeof(Vertex);
        tri.maxVertex                   = vertCount - 1;
        tri.indexType                   = VK_INDEX_TYPE_UINT32;
        tri.indexData.deviceAddress     = ib->GetDeviceAddress();
        tri.transformData.deviceAddress = 0;

        VkAccelerationStructureGeometryKHR geom{ VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR };
        geom.geometryType       = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
        geom.geometry.triangles = tri;
        geom.flags              = VK_GEOMETRY_OPAQUE_BIT_KHR;

        const u32 primitiveCount = indexCount / 3;

        VkAccelerationStructureBuildGeometryInfoKHR buildInfo{
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR };
        buildInfo.type          = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        buildInfo.flags         = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
        buildInfo.mode          = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        buildInfo.geometryCount = 1;
        buildInfo.pGeometries   = &geom;

        VkAccelerationStructureBuildSizesInfoKHR sizes{
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR };
        rt.vkGetAccelerationStructureBuildSizesKHR(
            device,
            VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
            &buildInfo,
            &primitiveCount,
            &sizes);

        auto result = std::make_shared<VKAccelerationStructure>();

        // Persistent AS storage buffer.
        VkBufferCreateInfo storageCi{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        storageCi.size        = sizes.accelerationStructureSize;
        storageCi.usage       = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR
                              | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
        storageCi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        result->m_StorageAlloc = VulkanAllocator::AllocateBuffer(
            storageCi, VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, result->m_StorageBuffer);

        // One-shot scratch. ImmediateSubmit waits on the build fence before returning, so
        // PushDeletion runs only for ring hygiene — N+2 retirement is overkill but harmless.
        const u64 scratchAlign = ctx.GetAsProperties().minAccelerationStructureScratchOffsetAlignment;
        const u64 scratchSize  = AlignUp(sizes.buildScratchSize, scratchAlign);
        VkBufferCreateInfo scratchCi{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        scratchCi.size        = scratchSize;
        scratchCi.usage       = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
        scratchCi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VkBuffer scratchBuffer = VK_NULL_HANDLE;
        VmaAllocation scratchAlloc = VulkanAllocator::AllocateBuffer(
            scratchCi, VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, scratchBuffer);
        VkBufferDeviceAddressInfo scratchAddrInfo{ VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO };
        scratchAddrInfo.buffer = scratchBuffer;
        const VkDeviceAddress scratchBda = vkGetBufferDeviceAddress(device, &scratchAddrInfo);

        // Create the AS handle bound to the persistent storage buffer.
        VkAccelerationStructureCreateInfoKHR asCi{ VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR };
        asCi.buffer = result->m_StorageBuffer;
        asCi.offset = 0;
        asCi.size   = sizes.accelerationStructureSize;
        asCi.type   = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        rt.vkCreateAccelerationStructureKHR(device, &asCi, nullptr, &result->m_Handle);

        buildInfo.dstAccelerationStructure  = result->m_Handle;
        buildInfo.scratchData.deviceAddress = scratchBda;

        VkAccelerationStructureBuildRangeInfoKHR range{};
        range.primitiveCount  = primitiveCount;
        range.primitiveOffset = 0;
        range.firstVertex     = 0;
        range.transformOffset = 0;
        const VkAccelerationStructureBuildRangeInfoKHR* pRange = &range;

        ctx.ImmediateSubmit([&](VkCommandBuffer cmd) {
            rt.vkCmdBuildAccelerationStructuresKHR(cmd, 1, &buildInfo, &pRange);
        });

        VulkanContext::Get().PushDeletion([scratchBuffer, scratchAlloc]() {
            VulkanAllocator::FreeBuffer(scratchBuffer, scratchAlloc);
        });

        VkAccelerationStructureDeviceAddressInfoKHR addrInfo{
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR };
        addrInfo.accelerationStructure = result->m_Handle;
        result->m_DeviceAddress = rt.vkGetAccelerationStructureDeviceAddressKHR(device, &addrInfo);

        LH_CORE_TRACE("BLAS built ({} verts, {} tris, AS size={} B)",
                      vertCount, primitiveCount, sizes.accelerationStructureSize);
        return result;
    }
}
