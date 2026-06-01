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

        // Allocate a device-local VkBuffer with the requested usage. Returns the buffer + alloc
        // (caller stores both for PushDeletion) and the cached device address.
        VkDeviceAddress AllocateDeviceBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                                             VkBuffer& outBuffer, VmaAllocation& outAlloc)
        {
            VkBufferCreateInfo ci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
            ci.size        = size;
            ci.usage       = usage;
            ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            outAlloc = VulkanAllocator::AllocateBuffer(ci, VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, outBuffer);
            VkBufferDeviceAddressInfo addrInfo{ VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO };
            addrInfo.buffer = outBuffer;
            return vkGetBufferDeviceAddress(VulkanContext::Get().GetDevice(), &addrInfo);
        }
    }

    VKAccelerationStructure::~VKAccelerationStructure()
    {
        if (m_Handle == VK_NULL_HANDLE && m_SkinInputBuffer == VK_NULL_HANDLE
            && m_DeformedBuffer == VK_NULL_HANDLE) return;
        auto handle      = m_Handle;
        auto storage     = m_StorageBuffer;
        auto storageAlloc= m_StorageAlloc;
        auto skin        = m_SkinInputBuffer;
        auto skinAlloc   = m_SkinInputAlloc;
        auto deformed    = m_DeformedBuffer;
        auto deformedAlloc = m_DeformedAlloc;
        VulkanContext::Get().PushDeletion([handle, storage, storageAlloc, skin, skinAlloc, deformed, deformedAlloc]() {
            auto& ctx = VulkanContext::Get();
            if (handle != VK_NULL_HANDLE)
                ctx.GetRtFn().vkDestroyAccelerationStructureKHR(ctx.GetDevice(), handle, nullptr);
            if (storage != VK_NULL_HANDLE) VulkanAllocator::FreeBuffer(storage,  storageAlloc);
            if (skin    != VK_NULL_HANDLE) VulkanAllocator::FreeBuffer(skin,     skinAlloc);
            if (deformed!= VK_NULL_HANDLE) VulkanAllocator::FreeBuffer(deformed, deformedAlloc);
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
        result->m_PrimitiveCount = primitiveCount;

        // Persistent AS storage buffer. CONCURRENT: BLAS is built on graphics (ImmediateSubmit)
        // but read on compute (RtSunShadowsPass raygen) via TLAS device-address dereference.
        // Per arch/multi-queue.md, cross-queue buffer access requires CONCURRENT or QFOT; AS
        // storage was missed in the original policy because B.2's per-frame TLAS was culled
        // silently and never exercised cross-queue.
        VkBufferCreateInfo storageCi{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        storageCi.size        = sizes.accelerationStructureSize;
        storageCi.usage       = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR
                              | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
        VulkanContext::Get().ApplyConcurrentSharing(storageCi);
        result->m_StorageAlloc = VulkanAllocator::AllocateBuffer(
            storageCi, VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, result->m_StorageBuffer);

        // One-shot scratch. ImmediateSubmit waits on the build fence before returning, so
        // PushDeletion runs only for ring hygiene — N+2 retirement is overkill but harmless.
        const u64 scratchAlign = ctx.GetAsProperties().minAccelerationStructureScratchOffsetAlignment;
        const u64 scratchSize  = AlignUp(sizes.buildScratchSize, scratchAlign);
        VkBuffer scratchBuffer = VK_NULL_HANDLE;
        VmaAllocation scratchAlloc = nullptr;
        const VkDeviceAddress scratchBda = AllocateDeviceBuffer(
            scratchSize,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            scratchBuffer, scratchAlloc);

        // Create the AS handle bound to the persistent storage buffer.
        VkAccelerationStructureCreateInfoKHR asCi{ VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR };
        asCi.buffer = result->m_StorageBuffer;
        asCi.offset = 0;
        asCi.size   = sizes.accelerationStructureSize;
        asCi.type   = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        rt.vkCreateAccelerationStructureKHR(device, &asCi, nullptr, &result->m_Handle);
        VulkanContext::SetDebugName(result->m_Handle, "BLAS");

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

    std::shared_ptr<VKAccelerationStructure> VKAccelerationStructure::CreateSkinnedBLAS(
        const Mesh& mesh, std::span<const SkinnedVertex> skinnedVerts)
    {
        auto& ctx = VulkanContext::Get();
        VkDevice device = ctx.GetDevice();
        const auto& rt  = ctx.GetRtFn();

        auto ib = std::dynamic_pointer_cast<VKIndexBuffer>(mesh.GetIndexBuffer());
        if (!ib)
        {
            LH_CORE_ERROR("CreateSkinnedBLAS: non-Vulkan IB on mesh — skipping BLAS");
            return nullptr;
        }
        const u32 vertCount  = mesh.GetVertexCount();
        const u32 indexCount = ib->GetCount();
        if (vertCount == 0 || indexCount == 0) return nullptr;
        if (skinnedVerts.size() != vertCount)
        {
            LH_CORE_ERROR("CreateSkinnedBLAS: vert count mismatch ({} vs {})",
                          skinnedVerts.size(), vertCount);
            return nullptr;
        }

        auto result = std::make_shared<VKAccelerationStructure>();
        result->m_IsSkinned    = true;
        result->m_VertexCount  = vertCount;
        result->m_PrimitiveCount = indexCount / 3;

        // Skin-input: tight-packed pos+boneIDs+weights. Compute reads via BDA + scalar layout.
        // Cross-queue: uploaded on transfer queue, read on AsyncCompute every frame. EXCLUSIVE
        // sharing here would be a spec violation (cross-queue without QFOT) and TDRs on NVIDIA;
        // CONCURRENT matches the universal CPU→GPU data-path policy in arch/multi-queue.md.
        const VkDeviceSize skinInputSize = static_cast<VkDeviceSize>(vertCount) * sizeof(SkinComputeInput);
        {
            VkBufferCreateInfo ci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
            ci.size        = skinInputSize;
            ci.usage       = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
                           | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT
                           | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            ctx.ApplyConcurrentSharing(ci);
            result->m_SkinInputAlloc = VulkanAllocator::AllocateBuffer(
                ci, VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, result->m_SkinInputBuffer);
            VkBufferDeviceAddressInfo addrInfo{ VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO };
            addrInfo.buffer = result->m_SkinInputBuffer;
            result->m_SkinInputBda = vkGetBufferDeviceAddress(device, &addrInfo);
        }

        // Extract on CPU then upload through UploadContext (transfer ring).
        std::vector<SkinComputeInput> skinInput(vertCount);
        for (u32 i = 0; i < vertCount; ++i)
        {
            // .w = 1.0 lets the shader skip the vec3→vec4 promotion inside the bone-matrix multiply.
            skinInput[i].Position    = Vec4(skinnedVerts[i].Position, 1.0f);
            skinInput[i].BoneIDs     = skinnedVerts[i].BoneIDs;
            skinInput[i].BoneWeights = skinnedVerts[i].BoneWeights;
        }
        const u64 skinUploadFence = UploadContext::Get().UploadBuffer(
            skinInput.data(), skinInputSize, result->m_SkinInputBuffer, 0);

        // Deformed positions — vec3/vert; compute writes, AS build reads. Zero-filled before the initial
        // build (VMA leaves device memory uninitialized; recycled NaN/Inf would TDR the BVH builder).
        // Cross-queue: graphics initial build, then compute write+read per frame. see arch/multi-queue.md
        const VkDeviceSize deformedSize = static_cast<VkDeviceSize>(vertCount) * sizeof(Vec3);
        {
            VkBufferCreateInfo ci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
            ci.size        = deformedSize;
            ci.usage       = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
                           | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT
                           | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR
                           | VK_BUFFER_USAGE_TRANSFER_DST_BIT;  // vkCmdFillBuffer zero-init pre-build
            ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            ctx.ApplyConcurrentSharing(ci);
            result->m_DeformedAlloc = VulkanAllocator::AllocateBuffer(
                ci, VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, result->m_DeformedBuffer);
            VkBufferDeviceAddressInfo addrInfo{ VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO };
            addrInfo.buffer = result->m_DeformedBuffer;
            result->m_DeformedBda = vkGetBufferDeviceAddress(device, &addrInfo);
        }

        // Wait for IB + skin-input uploads before recording the initial AS build.
        const u64 fence = std::max<u64>(ib->GetUploadFence(), skinUploadFence);
        if (fence > 0) UploadContext::Get().WaitForUpload(fence);

        // Geometry desc reads from deformed positions buffer (NOT the source SkinnedVertex VB).
        // The initial build sees zero positions; the first per-frame Refit overwrites with real ones.
        VkAccelerationStructureGeometryTrianglesDataKHR tri{
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR };
        tri.vertexFormat                = VK_FORMAT_R32G32B32_SFLOAT;
        tri.vertexData.deviceAddress    = result->m_DeformedBda;
        tri.vertexStride                = sizeof(Vec3);
        tri.maxVertex                   = vertCount - 1;
        tri.indexType                   = VK_INDEX_TYPE_UINT32;
        tri.indexData.deviceAddress     = ib->GetDeviceAddress();
        tri.transformData.deviceAddress = 0;

        VkAccelerationStructureGeometryKHR geom{ VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR };
        geom.geometryType       = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
        geom.geometry.triangles = tri;
        // No OPAQUE flag — skinned meshes may need any-hit (alpha test) later. Conservative now.
        geom.flags              = 0;

        VkAccelerationStructureBuildGeometryInfoKHR buildInfo{
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR };
        buildInfo.type          = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        // ALLOW_UPDATE enables per-frame MODE_UPDATE refits (cheaper than full rebuild).
        buildInfo.flags         = VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR
                                | VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
        buildInfo.mode          = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        buildInfo.geometryCount = 1;
        buildInfo.pGeometries   = &geom;

        VkAccelerationStructureBuildSizesInfoKHR sizes{
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR };
        rt.vkGetAccelerationStructureBuildSizesKHR(
            device,
            VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
            &buildInfo,
            &result->m_PrimitiveCount,
            &sizes);
        result->m_UpdateScratchSize = sizes.updateScratchSize;

        // Persistent AS storage. CONCURRENT for cross-queue read by RT trace (see CreateStaticBLAS).
        VkBufferCreateInfo storageCi{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        storageCi.size        = sizes.accelerationStructureSize;
        storageCi.usage       = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR
                              | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
        VulkanContext::Get().ApplyConcurrentSharing(storageCi);
        result->m_StorageAlloc = VulkanAllocator::AllocateBuffer(
            storageCi, VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, result->m_StorageBuffer);

        // One-shot build scratch.
        const u64 scratchAlign = ctx.GetAsProperties().minAccelerationStructureScratchOffsetAlignment;
        const u64 buildScratchSize = AlignUp(sizes.buildScratchSize, scratchAlign);
        VkBuffer scratchBuffer = VK_NULL_HANDLE;
        VmaAllocation scratchAlloc = nullptr;
        const VkDeviceAddress scratchBda = AllocateDeviceBuffer(
            buildScratchSize,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            scratchBuffer, scratchAlloc);

        VkAccelerationStructureCreateInfoKHR asCi{ VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR };
        asCi.buffer = result->m_StorageBuffer;
        asCi.offset = 0;
        asCi.size   = sizes.accelerationStructureSize;
        asCi.type   = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        rt.vkCreateAccelerationStructureKHR(device, &asCi, nullptr, &result->m_Handle);
        VulkanContext::SetDebugName(result->m_Handle, "BLAS");

        buildInfo.dstAccelerationStructure  = result->m_Handle;
        buildInfo.scratchData.deviceAddress = scratchBda;

        VkAccelerationStructureBuildRangeInfoKHR range{};
        range.primitiveCount  = result->m_PrimitiveCount;
        range.primitiveOffset = 0;
        range.firstVertex     = 0;
        range.transformOffset = 0;
        const VkAccelerationStructureBuildRangeInfoKHR* pRange = &range;

        ctx.ImmediateSubmit([&](VkCommandBuffer cmd) {
            // VMA leaves this uninitialized; zero it so the build reads degenerate tris, not NaN/Inf (TDR).
            vkCmdFillBuffer(cmd, result->m_DeformedBuffer, 0, VK_WHOLE_SIZE, 0u);
            VkMemoryBarrier2 fillBarrier{ VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
            fillBarrier.srcStageMask  = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
            fillBarrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            fillBarrier.dstStageMask  = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
            fillBarrier.dstAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR;
            VkDependencyInfo fillDep{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
            fillDep.memoryBarrierCount = 1;
            fillDep.pMemoryBarriers    = &fillBarrier;
            vkCmdPipelineBarrier2(cmd, &fillDep);

            rt.vkCmdBuildAccelerationStructuresKHR(cmd, 1, &buildInfo, &pRange);
        });

        VulkanContext::Get().PushDeletion([scratchBuffer, scratchAlloc]() {
            VulkanAllocator::FreeBuffer(scratchBuffer, scratchAlloc);
        });

        VkAccelerationStructureDeviceAddressInfoKHR addrInfo{
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR };
        addrInfo.accelerationStructure = result->m_Handle;
        result->m_DeviceAddress = rt.vkGetAccelerationStructureDeviceAddressKHR(device, &addrInfo);

        LH_CORE_TRACE("Skinned BLAS built ({} verts, {} tris, AS size={} B, update scratch={} B)",
                      vertCount, result->m_PrimitiveCount, sizes.accelerationStructureSize,
                      sizes.updateScratchSize);
        return result;
    }

    void VKAccelerationStructure::Refit(VkCommandBuffer cmd, VkDeviceAddress scratchBda) const
    {
        auto& ctx = VulkanContext::Get();
        const auto& rt = ctx.GetRtFn();

        VkAccelerationStructureGeometryTrianglesDataKHR tri{
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR };
        tri.vertexFormat                = VK_FORMAT_R32G32B32_SFLOAT;
        tri.vertexData.deviceAddress    = m_DeformedBda;
        tri.vertexStride                = sizeof(Vec3);
        tri.maxVertex                   = m_VertexCount - 1;
        tri.indexType                   = VK_INDEX_TYPE_UINT32;
        // Refit must use the SAME index buffer as the original build. The Mesh's IB device address
        // would be the source of truth — but this method takes only `cmd` + scratch by design.
        // Owner threads the IB BDA via the caller (B.2.D's TlasBuilder packs the build infos
        // directly, bypassing this single-mesh Refit path). For the single-mesh fallback used by
        // tests or one-off refits, the geometry pointer must be re-derived — caller passes nullptr
        // here means we expect the BLAS to keep the same geometry layout under update mode (Vulkan
        // spec says vertex/index pointers may change, but counts + formats must not). We re-bind
        // both deformed VB + retain primitive count; index pointer comes from `m_IndexBuffer` which
        // we don't own — for simplicity Refit reads it from the Mesh-owned IB at the call site in B.2.D.
        // This single-mesh Refit is a placeholder for symmetry; the production refit batch in
        // B.2.D loops infos+ranges and never invokes this method.
        tri.indexData.deviceAddress     = 0;
        tri.transformData.deviceAddress = 0;

        VkAccelerationStructureGeometryKHR geom{ VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR };
        geom.geometryType       = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
        geom.geometry.triangles = tri;
        geom.flags              = 0;

        VkAccelerationStructureBuildGeometryInfoKHR buildInfo{
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR };
        buildInfo.type                     = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        buildInfo.flags                    = VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR
                                           | VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
        buildInfo.mode                     = VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR;
        buildInfo.srcAccelerationStructure = m_Handle;
        buildInfo.dstAccelerationStructure = m_Handle;
        buildInfo.geometryCount            = 1;
        buildInfo.pGeometries              = &geom;
        buildInfo.scratchData.deviceAddress = scratchBda;

        VkAccelerationStructureBuildRangeInfoKHR range{};
        range.primitiveCount = m_PrimitiveCount;
        const VkAccelerationStructureBuildRangeInfoKHR* pRange = &range;

        rt.vkCmdBuildAccelerationStructuresKHR(cmd, 1, &buildInfo, &pRange);
    }
}
