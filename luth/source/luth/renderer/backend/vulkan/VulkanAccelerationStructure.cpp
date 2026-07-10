#include "luthpch.h"
#include "VulkanAccelerationStructure.h"
#include "VulkanContext.h"
#include "VulkanBuffer.h"
#include "UploadContext.h"
#include "luth/renderer/resources/Mesh.h"
#include "luth/renderer/resources/Model.h"
#include "luth/core/diagnostics/Log.h"
#include "luth/jobs/SpinLock.h"

#include <vma/vk_mem_alloc.h>
#include <algorithm>
#include <vector>

namespace Luth
{
    namespace
    {
        constexpr u64 AlignUp(u64 value, u64 alignment)
        {
            return (value + (alignment - 1)) & ~(alignment - 1);
        }

        // Static BLAS builds queued by CreateStaticBLAS (main thread) and drained on the async-compute AS
        // pass (render worker). SpinLock, not std::mutex: the drain runs inside a RecordingScope where a
        // fiber must not yield on OS sync (fiber-system.md V1/V3). weak_ptr so an evicted mesh cancels.
        struct PendingStaticBuild
        {
            std::weak_ptr<VKAccelerationStructure> as;
            u64                                    vbIbFence;
        };
        std::vector<PendingStaticBuild> g_PendingStatic;
        SpinLock                        g_PendingStaticLock;
    }

    // The deformed vertex buffer is the interleaved Vertex layout, written field-by-field as 18
    // hardcoded floats in skinning.slang and read at the same offsets by material.slang's geometry
    // table; lock the layout so a Vertex field reorder/resize can't silently desync the shaders.
    static_assert(sizeof(Vertex)              == 72, "deformed-vertex ABI: Vertex must stay 18 tight floats");
    static_assert(offsetof(Vertex, Position)  == 0,  "deformed-vertex ABI: pos @ float 0");
    static_assert(offsetof(Vertex, Normal)    == 12, "deformed-vertex ABI: normal @ float 3");
    static_assert(offsetof(Vertex, TexCoord0) == 24, "deformed-vertex ABI: uv0 @ float 6");
    static_assert(offsetof(Vertex, TexCoord1) == 32, "deformed-vertex ABI: uv1 @ float 8");
    static_assert(offsetof(Vertex, Tangent)   == 40, "deformed-vertex ABI: tangent @ float 10");
    static_assert(offsetof(Vertex, Color)     == 56, "deformed-vertex ABI: color @ float 14");

    // skinning.slang reads the source SkinnedVertex VB directly via scalar buffer_reference; lock the
    // tight 104 B layout so a field reorder/resize can't silently desync the compute's input fetch.
    static_assert(sizeof(SkinnedVertex)                == 104, "skin-input ABI: SkinnedVertex must stay tight 104 B");
    static_assert(offsetof(SkinnedVertex, Position)    == 0,   "skin-input ABI: pos @ 0");
    static_assert(offsetof(SkinnedVertex, Normal)      == 12,  "skin-input ABI: normal @ 12");
    static_assert(offsetof(SkinnedVertex, TexCoord0)   == 24,  "skin-input ABI: uv0 @ 24");
    static_assert(offsetof(SkinnedVertex, TexCoord1)   == 32,  "skin-input ABI: uv1 @ 32");
    static_assert(offsetof(SkinnedVertex, Tangent)     == 40,  "skin-input ABI: tangent @ 40");
    static_assert(offsetof(SkinnedVertex, Color)       == 56,  "skin-input ABI: color @ 56");
    static_assert(offsetof(SkinnedVertex, BoneIDs)     == 72,  "skin-input ABI: boneIDs @ 72");
    static_assert(offsetof(SkinnedVertex, BoneWeights) == 88,  "skin-input ABI: weights @ 88");

    VKAccelerationStructure::~VKAccelerationStructure()
    {
        if (m_Handle == VK_NULL_HANDLE && m_DeformedBuffer == VK_NULL_HANDLE) return;
        auto handle      = m_Handle;
        auto storage     = m_StorageBuffer;
        auto storageAlloc= m_StorageAlloc;
        auto deformed    = m_DeformedBuffer;
        auto deformedAlloc = m_DeformedAlloc;
        VulkanContext::Get().PushDeletion([handle, storage, storageAlloc, deformed, deformedAlloc]() {
            auto& ctx = VulkanContext::Get();
            if (handle != VK_NULL_HANDLE)
                ctx.GetRtFn().vkDestroyAccelerationStructureKHR(ctx.GetDevice(), handle, nullptr);
            if (storage != VK_NULL_HANDLE) VulkanAllocator::FreeBuffer(storage,  storageAlloc);
            if (deformed!= VK_NULL_HANDLE) VulkanAllocator::FreeBuffer(deformed, deformedAlloc);
        });
    }

    void VKAccelerationStructure::RecordBuild(VkCommandBuffer cmd, VkDeviceAddress scratchBda, u32 frameAbs)
    {
        const auto& rt = VulkanContext::Get().GetRtFn();

        // Deformable builds over its per-frame CURR deformed region (written by the deform compute earlier
        // this frame); static builds over its fixed source VB. Index/stride/flags are the recipe captured
        // at creation.
        //
        // invariant: vkCmdBuildAccelerationStructuresKHR retains pointers into the geometry/build-info
        // structs until the GPU executes the build (mirrors TlasBuilder::RefitSkinnedBLASes), so heap-
        // allocate them to outlive a deferred command buffer; free via PushDeletion (N+2).
        struct BuildCtx
        {
            VkAccelerationStructureGeometryKHR              geom;
            VkAccelerationStructureBuildGeometryInfoKHR     info;
            VkAccelerationStructureBuildRangeInfoKHR        range;
            const VkAccelerationStructureBuildRangeInfoKHR* rangePtr;
        };
        auto* bc = new BuildCtx{};

        bc->geom = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR };
        bc->geom.geometryType                            = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
        bc->geom.geometry.triangles.sType                = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
        bc->geom.geometry.triangles.vertexFormat         = VK_FORMAT_R32G32B32_SFLOAT;
        bc->geom.geometry.triangles.vertexData.deviceAddress = m_IsDeformable ? GetDeformedBdaCurr(frameAbs) : m_BuildVbBda;
        bc->geom.geometry.triangles.vertexStride         = m_BuildVertexStride;
        bc->geom.geometry.triangles.maxVertex            = m_BuildMaxVertex;
        bc->geom.geometry.triangles.indexType            = VK_INDEX_TYPE_UINT32;
        bc->geom.geometry.triangles.indexData.deviceAddress = m_BuildIbBda;
        bc->geom.flags                                   = m_GeomFlags;

        bc->info = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR };
        bc->info.type                     = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        bc->info.flags                    = m_BuildFlags;
        bc->info.mode                     = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        bc->info.dstAccelerationStructure = m_Handle;
        bc->info.geometryCount            = 1;
        bc->info.pGeometries              = &bc->geom;
        bc->info.scratchData.deviceAddress = scratchBda;

        bc->range    = { m_PrimitiveCount, 0u, 0u, 0u };
        bc->rangePtr = &bc->range;

        rt.vkCmdBuildAccelerationStructuresKHR(cmd, 1, &bc->info, &bc->rangePtr);
        VulkanContext::Get().PushDeletion([bc]() { delete bc; });

        m_BuildRecorded = true;
        m_BuildFrameAbs = frameAbs;
    }

    u32 VKAccelerationStructure::DrainPendingStaticBuilds(VkCommandBuffer cmd, u32 frameAbs)
    {
        std::vector<PendingStaticBuild> pending;
        {
            SpinLockGuard lock(g_PendingStaticLock);
            if (g_PendingStatic.empty()) return 0;
            pending.swap(g_PendingStatic);   // drain a snapshot; concurrent enqueues stay for next frame
        }

        auto& ctx = VulkanContext::Get();

        // Partition: upload retired -> build this frame; still uploading -> requeue; mesh evicted -> drop.
        struct Ready { std::shared_ptr<VKAccelerationStructure> as; u64 scratchOffset; };
        std::vector<Ready> ready;
        std::vector<PendingStaticBuild> requeue;
        u64 totalScratch = 0;
        for (auto& rec : pending)
        {
            auto as = rec.as.lock();
            if (!as) continue;   // mesh evicted before its build: cancel (the AS is already destroyed)
            if (!UploadContext::Get().IsComplete(rec.vbIbFence)) { requeue.push_back(rec); continue; }
            ready.push_back({ as, totalScratch });
            totalScratch += as->GetBuildScratchSize();
        }

        u32 built = 0;
        if (!ready.empty())
        {
            // One batched device-local scratch, per-build sub-region (offsets are pre-aligned scratch sizes).
            // DEVICE_LOCAL, not the HOST_VISIBLE tagged heap (NVIDIA's RT accelerator TDRs on it); the base
            // is retired N+2 via PushDeletion. Mirrors TlasBuilder::RefitSkinnedBLASes. see arch/memory.md
            VkBufferCreateInfo scratchCi{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
            scratchCi.size        = totalScratch;
            scratchCi.usage       = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
            scratchCi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            VkBuffer scratchBuf = VK_NULL_HANDLE;
            VmaAllocation scratchAlloc = VulkanAllocator::AllocateBuffer(
                scratchCi, VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, scratchBuf);
            if (scratchBuf)
            {
                VkBufferDeviceAddressInfo addrInfo{ VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO };
                addrInfo.buffer = scratchBuf;
                const VkDeviceAddress scratchBase = vkGetBufferDeviceAddress(ctx.GetDevice(), &addrInfo);
                for (auto& r : ready)
                    r.as->RecordBuild(cmd, scratchBase + r.scratchOffset, frameAbs);
                built = static_cast<u32>(ready.size());
                VulkanContext::Get().PushDeletion([scratchBuf, scratchAlloc]() {
                    VulkanAllocator::FreeBuffer(scratchBuf, scratchAlloc);
                });
            }
            else
            {
                // Scratch OOM this frame: requeue (fence 0 == already uploaded, so IsComplete stays true).
                for (auto& r : ready) requeue.push_back({ r.as, 0u });
            }
        }

        if (!requeue.empty())
        {
            SpinLockGuard lock(g_PendingStaticLock);
            g_PendingStatic.insert(g_PendingStatic.end(), requeue.begin(), requeue.end());
        }
        return built;
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
            LH_LOG(Renderer, error, "CreateStaticBLAS: non-Vulkan VB/IB on mesh - skipping BLAS");
            return nullptr;
        }
        const u32 vertCount  = mesh.GetVertexCount();
        const u32 indexCount = ib->GetCount();
        if (vertCount == 0 || indexCount == 0) return nullptr;

        // Position is at offset 0 in both Vertex and SkinnedVertex. Stride keeps the rest of the
        // vertex fields skipped naturally; AS-build only reads positions per the geometry desc.
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

        // Persistent AS storage buffer. CONCURRENT: built + read on the async-compute AS pass, and read by the
        // RT trace via the TLAS device-address dereference; cross-queue access needs CONCURRENT or QFOT
        // (arch/multi-queue.md).
        VkBufferCreateInfo storageCi{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        storageCi.size        = sizes.accelerationStructureSize;
        storageCi.usage       = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR
                              | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
        VulkanContext::Get().ApplyConcurrentSharing(storageCi);
        result->m_StorageAlloc = VulkanAllocator::AllocateBuffer(
            storageCi, VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, result->m_StorageBuffer);

        // Scratch is allocated at drain time (batched across the frame's ready builds); record its size.
        const u64 scratchAlign = ctx.GetAsProperties().minAccelerationStructureScratchOffsetAlignment;
        const u64 scratchSize  = AlignUp(sizes.buildScratchSize, scratchAlign);

        // Create the AS handle bound to the persistent storage buffer.
        VkAccelerationStructureCreateInfoKHR asCi{ VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR };
        asCi.buffer = result->m_StorageBuffer;
        asCi.offset = 0;
        asCi.size   = sizes.accelerationStructureSize;
        asCi.type   = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        rt.vkCreateAccelerationStructureKHR(device, &asCi, nullptr, &result->m_Handle);
        VulkanContext::SetDebugName(result->m_Handle, "BLAS");

        // Capture the build recipe so RecordBuild can replay it from a deferred command buffer.
        result->m_BuildVbBda        = tri.vertexData.deviceAddress;
        result->m_BuildIbBda        = tri.indexData.deviceAddress;
        result->m_BuildVertexStride = static_cast<u32>(tri.vertexStride);
        result->m_BuildMaxVertex    = tri.maxVertex;
        result->m_BuildScratchSize  = scratchSize;
        result->m_GeomFlags         = geom.flags;
        result->m_BuildFlags        = buildInfo.flags;

        VkAccelerationStructureDeviceAddressInfoKHR addrInfo{
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR };
        addrInfo.accelerationStructure = result->m_Handle;
        result->m_DeviceAddress = rt.vkGetAccelerationStructureDeviceAddressKHR(device, &addrInfo);

        // Enqueue the build instead of blocking. The async-compute TLAS pass drains it once the VB/IB
        // upload fence retires (DrainPendingStaticBuilds); until then the BLAS is unbuilt and the TLAS
        // gather skips it. weak_ptr so a mesh evicted before its build cancels cleanly.
        {
            SpinLockGuard lock(g_PendingStaticLock);
            g_PendingStatic.push_back({ result, std::max(vb->GetUploadFence(), ib->GetUploadFence()) });
        }

        LH_LOG(Renderer, trace, "BLAS created, build deferred ({} verts, {} tris, AS size={} B)",
                      vertCount, primitiveCount, sizes.accelerationStructureSize);
        return result;
    }

    std::shared_ptr<VKAccelerationStructure> VKAccelerationStructure::CreateDeformableBLAS(const Mesh& mesh)
    {
        auto& ctx = VulkanContext::Get();
        VkDevice device = ctx.GetDevice();
        const auto& rt  = ctx.GetRtFn();

        auto vb = std::dynamic_pointer_cast<VKVertexBuffer>(mesh.GetVertexBuffer());
        auto ib = std::dynamic_pointer_cast<VKIndexBuffer>(mesh.GetIndexBuffer());
        if (!vb || !ib)
        {
            LH_LOG(Renderer, error, "CreateDeformableBLAS: non-Vulkan VB/IB on mesh - skipping BLAS");
            return nullptr;
        }
        const u32 vertCount  = mesh.GetVertexCount();
        const u32 indexCount = ib->GetCount();
        if (vertCount == 0 || indexCount == 0) return nullptr;

        auto result = std::make_shared<VKAccelerationStructure>();
        result->m_IsDeformable = true;
        result->m_VertexCount  = vertCount;
        result->m_PrimitiveCount = indexCount / 3;

        // Deformed vertices: interleaved Vertex layout (52 B: pos/normal/uv0/uv1/tangent) so the RT
        // geometry table reads post-skin normals/tangents byte-identical to a static VB; the AS build
        // reads positions at offset 0. Double-buffered (curr/prev regions) so raster motion vectors can
        // read the previous frame's positions; region alternates by frame parity, region 0 == CURR on
        // frame 0. The first refit does the MODE_BUILD over CURR after the deform has written it (gated on
        // the source-VB upload), so no pre-build zero-fill is needed. see arch/multi-queue.md
        const VkDeviceSize deformedRegionBytes = static_cast<VkDeviceSize>(vertCount) * sizeof(Vertex);
        const VkDeviceSize deformedSize        = 2 * deformedRegionBytes;
        result->m_DeformedRegionBytes = deformedRegionBytes;
        {
            VkBufferCreateInfo ci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
            ci.size        = deformedSize;
            ci.usage       = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
                           | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT
                           | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
            ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            ctx.ApplyConcurrentSharing(ci);
            result->m_DeformedAlloc = VulkanAllocator::AllocateBuffer(
                ci, VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, result->m_DeformedBuffer);
            VkBufferDeviceAddressInfo addrInfo{ VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO };
            addrInfo.buffer = result->m_DeformedBuffer;
            result->m_DeformedBda = vkGetBufferDeviceAddress(device, &addrInfo);
        }

        // Geometry desc reads positions (offset 0) from the deformed vertex buffer (NOT the source
        // SkinnedVertex VB). The initial build sees zero positions; the first per-frame Refit
        // overwrites with real ones.
        VkAccelerationStructureGeometryTrianglesDataKHR tri{
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR };
        tri.vertexFormat                = VK_FORMAT_R32G32B32_SFLOAT;
        tri.vertexData.deviceAddress    = result->m_DeformedBda;
        tri.vertexStride                = sizeof(Vertex);
        tri.maxVertex                   = vertCount - 1;
        tri.indexType                   = VK_INDEX_TYPE_UINT32;
        tri.indexData.deviceAddress     = ib->GetDeviceAddress();
        tri.transformData.deviceAddress = 0;

        VkAccelerationStructureGeometryKHR geom{ VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR };
        geom.geometryType       = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
        geom.geometry.triangles = tri;
        // No OPAQUE flag; skinned meshes may need any-hit (alpha test) later. Conservative now.
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

        // Build scratch is allocated at refit time (the first refit does the MODE_BUILD); record its size.
        const u64 scratchAlign = ctx.GetAsProperties().minAccelerationStructureScratchOffsetAlignment;
        const u64 buildScratchSize = AlignUp(sizes.buildScratchSize, scratchAlign);

        VkAccelerationStructureCreateInfoKHR asCi{ VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR };
        asCi.buffer = result->m_StorageBuffer;
        asCi.offset = 0;
        asCi.size   = sizes.accelerationStructureSize;
        asCi.type   = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        rt.vkCreateAccelerationStructureKHR(device, &asCi, nullptr, &result->m_Handle);
        VulkanContext::SetDebugName(result->m_Handle, "BLAS");

        // Capture the build recipe. Deformable RecordBuild reads the CURR deformed region (not a source
        // VB), so m_BuildVbBda stays 0; index/stride/flags feed both the first build and later refits.
        result->m_BuildIbBda        = tri.indexData.deviceAddress;
        result->m_BuildVertexStride = static_cast<u32>(tri.vertexStride);
        result->m_BuildMaxVertex    = tri.maxVertex;
        result->m_BuildScratchSize  = buildScratchSize;
        result->m_GeomFlags         = geom.flags;
        result->m_BuildFlags        = buildInfo.flags;

        VkAccelerationStructureDeviceAddressInfoKHR addrInfo{
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR };
        addrInfo.accelerationStructure = result->m_Handle;
        result->m_DeviceAddress = rt.vkGetAccelerationStructureDeviceAddressKHR(device, &addrInfo);

        // No enqueue + no synchronous build: the first per-frame RefitSkinnedBLASes does the MODE_BUILD over
        // the deform's CURR region once the source VB/IB upload retires, then MODE_UPDATEs thereafter.
        LH_LOG(Renderer, trace, "Deformable BLAS created, build deferred ({} verts, {} tris, AS size={} B, update scratch={} B)",
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
        tri.vertexStride                = sizeof(Vertex);
        tri.maxVertex                   = m_VertexCount - 1;
        tri.indexType                   = VK_INDEX_TYPE_UINT32;
        // Refit must use the SAME index buffer as the original build, but this method takes only
        // `cmd` + scratch, so the IB BDA is left 0. Legal under update mode: the spec allows
        // vertex/index pointers to change as long as counts + formats do not. Placeholder kept for
        // symmetry only; the production refit batch (TlasBuilder::RefitSkinnedBLASes) packs its
        // build infos + ranges directly and never invokes this method.
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
