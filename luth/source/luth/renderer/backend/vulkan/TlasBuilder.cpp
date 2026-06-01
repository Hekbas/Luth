#include "luthpch.h"
#include "TlasBuilder.h"
#include "VulkanContext.h"
#include "VulkanAccelerationStructure.h"
#include "VulkanBuffer.h"
#include "luth/core/RenderSnapshot.h"
#include "luth/core/diagnostics/Log.h"
#include "luth/jobs/JobSystem.h"
#include "luth/memory/GPUTaggedPageAllocator.h"
#include "luth/renderer/resources/Model.h"
#include "luth/renderer/resources/Mesh.h"
#include "luth/resources/AssetManager.h"

#include <vma/vk_mem_alloc.h>
#include <cstring>

namespace Luth
{
    namespace
    {
        constexpr u64 AlignUp(u64 value, u64 alignment)
        {
            return (value + (alignment - 1)) & ~(alignment - 1);
        }

        // CONTRACT: row-major transpose of glm column-major. VkAccelerationStructureInstanceKHR's
        // transform field is a 3x4 row-major matrix (VkTransformMatrixKHR := float[3][4]); our
        // worldMatrix is glm::mat4 (column-major). A naive memcpy ships silently-wrong bounds —
        // validation layer doesn't catch it and rays miss geometry.
        VkTransformMatrixKHR ToVkTransform(const Mat4& m)
        {
            VkTransformMatrixKHR out{};
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 4; ++c)
                    out.matrix[r][c] = m[c][r];
            return out;
        }

        // Cheap u64 mix — translation-only hash. Catches the common motion case (entities sliding
        // around). Misses rotation-only changes on non-translating meshes — if RT shadows expose
        // this, bump to full 64-byte matrix hash; cost is still < 10 µs for ~100 instances.
        u64 HashInstances(std::span<const MeshDrawSnapshot> instances)
        {
            u64 h = 0xcbf29ce484222325ull; // FNV-1a basis
            for (const auto& inst : instances)
            {
                h ^= static_cast<u64>(inst.entity);
                h *= 0x100000001b3ull;
                // Translation row of the column-major matrix is the 4th column (m[3].xyz).
                u32 t0, t1, t2;
                std::memcpy(&t0, &inst.worldMatrix[3][0], 4);
                std::memcpy(&t1, &inst.worldMatrix[3][1], 4);
                std::memcpy(&t2, &inst.worldMatrix[3][2], 4);
                h ^= t0;            h *= 0x100000001b3ull;
                h ^= t1;            h *= 0x100000001b3ull;
                h ^= t2;            h *= 0x100000001b3ull;
                h ^= static_cast<u64>(inst.meshIndex);
                h *= 0x100000001b3ull;
            }
            return h;
        }

        struct ResolvedMesh
        {
            const Mesh*                      mesh = nullptr;
            const VKAccelerationStructure*   blas = nullptr;
        };

        ResolvedMesh Resolve(const MeshDrawSnapshot& inst)
        {
            ResolvedMesh r;
            auto model = AssetManager::GetAsset<Model>(inst.modelUUID);
            if (!model) return r;
            auto mesh = model->GetMesh(inst.meshIndex);
            if (!mesh) return r;
            r.mesh = mesh.get();
            r.blas = mesh->GetBlas().get();
            return r;
        }
    }

    void TlasBuilder::RefitSkinnedBLASes(VkCommandBuffer cmd,
                                         std::span<const MeshDrawSnapshot> instances,
                                         u32 /*frameAbs*/)
    {
        auto& ctx = VulkanContext::Get();
        const auto& rt = ctx.GetRtFn();

        // Pre-resolve + compute aggregate scratch size. Per NVIDIA: each BLAS build in a batched
        // call needs its own scratch sub-region; sum the per-mesh updateScratchSize aligned up.
        const u64 scratchAlign = ctx.GetAsProperties().minAccelerationStructureScratchOffsetAlignment;

        struct RefitEntry
        {
            const VKAccelerationStructure* blas;
            const Mesh*                    mesh;
            u64                            scratchOffset;
            u64                            scratchSize;
        };
        std::vector<RefitEntry> entries;
        entries.reserve(instances.size());
        u64 totalScratch = 0;
        for (const auto& inst : instances)
        {
            if (!inst.isSkinned) continue;
            ResolvedMesh r = Resolve(inst);
            if (!r.blas || !r.blas->IsSkinned()) continue;
            const u64 sz = AlignUp(r.blas->GetUpdateScratchSize(), scratchAlign);
            entries.push_back({ r.blas, r.mesh, totalScratch, sz });
            totalScratch += sz;
        }
        if (entries.empty()) return;

        // AS-build scratch must be DEVICE_LOCAL — the tagged heap is HOST_VISIBLE (the CPU->GPU data
        // path) and NVIDIA's RT accelerator TDRs on it; PushDeletion retires it N+2. see arch/memory.md
        VkBufferCreateInfo scratchCi{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        scratchCi.size        = totalScratch;
        scratchCi.usage       = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
                              | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
        scratchCi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VkBuffer scratchBuf = VK_NULL_HANDLE;
        VmaAllocation scratchAlloc = VulkanAllocator::AllocateBuffer(
            scratchCi, VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, scratchBuf);
        if (!scratchBuf) return;
        VkBufferDeviceAddressInfo addrInfo{ VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO };
        addrInfo.buffer = scratchBuf;
        const VkDeviceAddress scratchBase = vkGetBufferDeviceAddress(ctx.GetDevice(), &addrInfo);
        VulkanContext::Get().PushDeletion([scratchBuf, scratchAlloc]() {
            VulkanAllocator::FreeBuffer(scratchBuf, scratchAlloc);
        });

        // invariant: vkCmdBuildAccelerationStructuresKHR retains pointers into these structs until the
        // GPU executes the build, so heap-allocate them to outlive this stack frame; free via PushDeletion.
        struct RefitCtx
        {
            std::vector<VkAccelerationStructureBuildGeometryInfoKHR> infos;
            std::vector<VkAccelerationStructureGeometryKHR>          geoms;
            std::vector<VkAccelerationStructureBuildRangeInfoKHR>    ranges;
            std::vector<const VkAccelerationStructureBuildRangeInfoKHR*> rangePtrs;
        };
        auto* refitCtx = new RefitCtx;
        refitCtx->infos.resize(entries.size());
        refitCtx->geoms.resize(entries.size());
        refitCtx->ranges.resize(entries.size());
        refitCtx->rangePtrs.resize(entries.size());

        for (size_t i = 0; i < entries.size(); ++i)
        {
            const auto& e = entries[i];
            auto ib = std::dynamic_pointer_cast<VKIndexBuffer>(e.mesh->GetIndexBuffer());
            const u32 vertCount  = e.blas->GetVertexCount();
            const u32 primCount  = (ib ? ib->GetCount() : 0) / 3;

            auto& geom = refitCtx->geoms[i];
            geom = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR };
            geom.geometryType                            = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
            geom.geometry.triangles.sType                = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
            geom.geometry.triangles.vertexFormat         = VK_FORMAT_R32G32B32_SFLOAT;
            geom.geometry.triangles.vertexData.deviceAddress = e.blas->GetDeformedBda();
            geom.geometry.triangles.vertexStride         = sizeof(Vec3);
            geom.geometry.triangles.maxVertex            = vertCount - 1;
            geom.geometry.triangles.indexType            = VK_INDEX_TYPE_UINT32;
            geom.geometry.triangles.indexData.deviceAddress = ib ? ib->GetDeviceAddress() : 0;
            geom.flags                                   = 0;

            auto& info = refitCtx->infos[i];
            info = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR };
            info.type                     = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
            info.flags                    = VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR
                                          | VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
            info.mode                     = VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR;
            info.srcAccelerationStructure = e.blas->GetHandle();
            info.dstAccelerationStructure = e.blas->GetHandle();
            info.geometryCount            = 1;
            info.pGeometries              = &refitCtx->geoms[i];
            info.scratchData.deviceAddress = scratchBase + e.scratchOffset;

            refitCtx->ranges[i]    = { primCount, 0u, 0u, 0u };
            refitCtx->rangePtrs[i] = &refitCtx->ranges[i];
        }

        rt.vkCmdBuildAccelerationStructuresKHR(cmd,
                                               static_cast<u32>(refitCtx->infos.size()),
                                               refitCtx->infos.data(),
                                               refitCtx->rangePtrs.data());

        VulkanContext::Get().PushDeletion([refitCtx]() { delete refitCtx; });
    }

    TlasBuildResult TlasBuilder::BuildTlas(VkCommandBuffer cmd,
                                           std::span<const MeshDrawSnapshot> instances,
                                           u32 frameAbs,
                                           const TlasBuildResult& prev)
    {
        const u64 hash = HashInstances(instances);
        if (prev.tlas != VK_NULL_HANDLE && hash == prev.instanceHash)
        {
            TlasBuildResult r = prev;
            r.reused = true;
            return r;
        }

        auto& ctx = VulkanContext::Get();
        const auto& rt = ctx.GetRtFn();
        VkDevice device = ctx.GetDevice();

        // Pack instance buffer. One VkAccelerationStructureInstanceKHR per resolved mesh that
        // has a non-null BLAS — meshes still uploading or with build failures get skipped.
        std::vector<VkAccelerationStructureInstanceKHR> packed;
        packed.reserve(instances.size());
        for (const auto& inst : instances)
        {
            ResolvedMesh r = Resolve(inst);
            if (!r.blas || r.blas->GetDeviceAddress() == 0) continue;
            VkAccelerationStructureInstanceKHR vkInst{};
            vkInst.transform                              = ToVkTransform(inst.worldMatrix);
            vkInst.instanceCustomIndex                    = inst.entity & 0x00FFFFFFu;
            vkInst.mask                                   = 0xFF;
            vkInst.instanceShaderBindingTableRecordOffset = 0;
            vkInst.flags                                  = 0;
            vkInst.accelerationStructureReference         = r.blas->GetDeviceAddress();
            packed.push_back(vkInst);
        }

        TlasBuildResult result{};
        result.instanceHash  = hash;
        result.instanceCount = static_cast<u32>(packed.size());

        if (packed.empty())
        {
            // Empty TLAS — return null handle; Set 0 binding 6 will bind VK_NULL_HANDLE this frame.
            return result;
        }

        // Per-frame instance buffer (mapped-sequential write — small data, no benefit from staging).
        const VkDeviceSize instanceBytes = packed.size() * sizeof(VkAccelerationStructureInstanceKHR);
        VkBufferCreateInfo instCi{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        instCi.size  = instanceBytes;
        instCi.usage = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR
                     | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT
                     | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        instCi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VkBuffer instBuffer = VK_NULL_HANDLE;
        void* instMapped = nullptr;
        VmaAllocation instAlloc = VulkanAllocator::AllocateMappedSequentialBuffer(instCi, instBuffer, &instMapped);
        std::memcpy(instMapped, packed.data(), instanceBytes);
        VulkanAllocator::FlushSlice(instAlloc, 0, instanceBytes);
        VkBufferDeviceAddressInfo instAddr{ VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO };
        instAddr.buffer = instBuffer;
        const VkDeviceAddress instBda = vkGetBufferDeviceAddress(device, &instAddr);

        // Instance buffer retires N+2 — by then this frame's TLAS build has long completed.
        VulkanContext::Get().PushDeletion([instBuffer, instAlloc]() {
            VulkanAllocator::FreeBuffer(instBuffer, instAlloc);
        });

        // Per-build context heap-allocated so its lifetime extends past BuildTlas's stack frame.
        // Vulkan's vkCmdBuildAccelerationStructuresKHR may keep pointers to the geometry / build /
        // range structs until the command executes on the GPU; stack-local versions would die after
        // BuildTlas returns and subsequent passes' stack frames could clobber the bytes. The block
        // is freed via PushDeletion (N+2 frames out — same lifetime as the storage buffer).
        struct BuildCtx
        {
            VkAccelerationStructureBuildGeometryInfoKHR buildInfo{
                VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR };
            VkAccelerationStructureGeometryKHR          geom{
                VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR };
            VkAccelerationStructureBuildRangeInfoKHR    range{};
            const VkAccelerationStructureBuildRangeInfoKHR* pRange = nullptr;
        };
        auto* ctx_ = new BuildCtx;

        // Geometry desc for the TLAS — INSTANCES type points at our packed instance buffer.
        ctx_->geom.geometryType                          = VK_GEOMETRY_TYPE_INSTANCES_KHR;
        ctx_->geom.geometry.instances.sType              = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
        ctx_->geom.geometry.instances.arrayOfPointers    = VK_FALSE;
        ctx_->geom.geometry.instances.data.deviceAddress = instBda;
        ctx_->geom.flags                                 = 0;

        const u32 primitiveCount = static_cast<u32>(packed.size());

        ctx_->buildInfo.type          = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
        ctx_->buildInfo.flags         = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
        ctx_->buildInfo.mode          = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        ctx_->buildInfo.geometryCount = 1;
        ctx_->buildInfo.pGeometries   = &ctx_->geom;

        VkAccelerationStructureBuildSizesInfoKHR sizes{
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR };
        rt.vkGetAccelerationStructureBuildSizesKHR(
            device,
            VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
            &ctx_->buildInfo,
            &primitiveCount,
            &sizes);

        // Per-frame TLAS storage (VMA, PushDeletion — drains N+2). CONCURRENT so RtSunShadowsPass
        // raygen can read it from the AsyncCompute queue without a queue-family-ownership transfer.
        VkBufferCreateInfo storageCi{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        storageCi.size        = sizes.accelerationStructureSize;
        storageCi.usage       = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR
                              | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
        VulkanContext::Get().ApplyConcurrentSharing(storageCi);
        result.storageAlloc = VulkanAllocator::AllocateBuffer(
            storageCi, VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, result.storageBuffer);

        // AS-build scratch must be DEVICE_LOCAL — the tagged-heap path
        // (AllocateMappedSequentialBuffer) returns HOST_VISIBLE memory, the documented CPU→GPU
        // data path; wrong tool for GPU-only scratch (NVIDIA's RT hardware accelerator can TDR
        // on HOST_VISIBLE scratch). Mirrors BuildEmptyTlas's pattern. PushDeletion N+2 retires
        // it on the same schedule as the persistent instance buffer.
        const u64 scratchAlign = ctx.GetAsProperties().minAccelerationStructureScratchOffsetAlignment;
        const u64 scratchSize  = AlignUp(sizes.buildScratchSize, scratchAlign);
        VkBufferCreateInfo scratchCi{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        scratchCi.size        = scratchSize;
        scratchCi.usage       = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
                              | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
        scratchCi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VkBuffer scratchBuf = VK_NULL_HANDLE;
        VmaAllocation scratchAlloc = VulkanAllocator::AllocateBuffer(
            scratchCi, VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, scratchBuf);
        if (!scratchBuf) return result;
        VkBufferDeviceAddressInfo scratchAddr{ VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO };
        scratchAddr.buffer = scratchBuf;
        const VkDeviceAddress scratchBda = vkGetBufferDeviceAddress(device, &scratchAddr);
        VulkanContext::Get().PushDeletion([scratchBuf, scratchAlloc]() {
            VulkanAllocator::FreeBuffer(scratchBuf, scratchAlloc);
        });

        // Create TLAS handle bound to per-frame storage.
        VkAccelerationStructureCreateInfoKHR asCi{ VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR };
        asCi.buffer = result.storageBuffer;
        asCi.offset = 0;
        asCi.size   = sizes.accelerationStructureSize;
        asCi.type   = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
        rt.vkCreateAccelerationStructureKHR(device, &asCi, nullptr, &result.tlas);
        VulkanContext::SetDebugName(result.tlas, "TLAS");

        ctx_->buildInfo.dstAccelerationStructure  = result.tlas;
        ctx_->buildInfo.scratchData.deviceAddress = scratchBda;

        ctx_->range.primitiveCount = primitiveCount;
        ctx_->pRange = &ctx_->range;
        rt.vkCmdBuildAccelerationStructuresKHR(cmd, 1, &ctx_->buildInfo, &ctx_->pRange);

        // Heap context retires N+2 frames out — by then the GPU has long finished the build.
        VulkanContext::Get().PushDeletion([ctx_]() { delete ctx_; });

        return result;
    }
}
