#include "luthpch.h"
#include "MaterialSystem.h"
#include "luth/renderer/Renderer.h"
#include "luth/renderer/backend/vulkan/VulkanContext.h"
#include "luth/core/FrameData.h"
#include "luth/core/diagnostics/Log.h"
#include "luth/jobs/JobSystem.h"
#include "luth/memory/GPUTaggedPageAllocator.h"
#include "luth/renderer/material/MaterialLayoutGuard.h"
#include "luth/resources/FileSystem.h"

namespace Luth
{
    VkDescriptorPool      MaterialSystem::m_DescriptorPool      = VK_NULL_HANDLE;
    VkDescriptorSetLayout MaterialSystem::m_DescriptorSetLayout = VK_NULL_HANDLE;
    std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> MaterialSystem::m_DescriptorSets{};

    std::vector<MaterialSystem::MaterialSlot> MaterialSystem::m_Slots;
    std::deque<u32> MaterialSystem::m_FreeIndices;
    Luth::SpinLock MaterialSystem::m_Lock;

    void MaterialSystem::Init()
    {
        CreateDescriptors();

        m_Slots.resize(MAX_MATERIALS);
        for (u32 i = 0; i < MAX_MATERIALS; ++i)
            m_FreeIndices.push_back(i);

        // Loud init-time guard: GPUMaterialData must stay byte-identical to material.slang's std430 mirror;
        // a silent stride drift corrupts every material index > 0. Field names match the Slang struct.
        static constexpr MaterialLayoutGuard::CppField kFields[] = {
            { "color",           offsetof(GPUMaterialData, color) },
            { "diffuseIndex",    offsetof(GPUMaterialData, diffuseIndex) },
            { "normalIndex",     offsetof(GPUMaterialData, normalIndex) },
            { "metalRoughIndex", offsetof(GPUMaterialData, metalRoughIndex) },
            { "occlusionIndex",  offsetof(GPUMaterialData, occlusionIndex) },
            { "emissiveIndex",   offsetof(GPUMaterialData, emissiveIndex) },
            { "alphaIndex",      offsetof(GPUMaterialData, alphaIndex) },
            { "heightIndex",     offsetof(GPUMaterialData, heightIndex) },
            { "thicknessIndex",  offsetof(GPUMaterialData, thicknessIndex) },
            { "metalness",       offsetof(GPUMaterialData, metalness) },
            { "roughness",       offsetof(GPUMaterialData, roughness) },
            { "alphaCutoff",     offsetof(GPUMaterialData, alphaCutoff) },
            { "flags",           offsetof(GPUMaterialData, flags) },
            { "emissive",        offsetof(GPUMaterialData, emissive) },
        };
        MaterialLayoutGuard::Validate(FileSystem::EngineAssetsPath("shaders/common/material.slang"),
                                      "GPUMaterialData", kFields, sizeof(GPUMaterialData));

        LH_LOG(Renderer, info, "Material System Initialized (Max Materials: {0})", MAX_MATERIALS);
    }

    void MaterialSystem::Shutdown()
    {
        // Release shared_ptr references to materials before destroying GPU resources
        m_Slots.clear();
        m_FreeIndices.clear();

        VkDevice device = VulkanContext::Get().GetDevice();
        vkDestroyDescriptorPool(device, m_DescriptorPool, nullptr);
        vkDestroyDescriptorSetLayout(device, m_DescriptorSetLayout, nullptr);
    }

    u32 MaterialSystem::RegisterMaterial(std::shared_ptr<Material> material)
    {
        LH_PROFILE_FUNCTION();
        // Slot mutation must run on the game stage; concurrent Render(N-1) reads the slot map without locking.
        assert(JobSystem::GetCurrentStage() == JobSystem::Stage::Game &&
            "MaterialSystem::RegisterMaterial must run on the game stage");
        SpinLockGuard lock(m_Lock);

        if (m_FreeIndices.empty())
        {
            LH_LOG(Renderer, error, "Material System: Out of slots!");
            return 0;
        }

        u32 index = m_FreeIndices.front();
        m_FreeIndices.pop_front();
        m_Slots[index].material = material;
        return index;
    }

    void MaterialSystem::UnregisterMaterial(u32 index)
    {
        LH_PROFILE_FUNCTION();
        assert(JobSystem::GetCurrentStage() == JobSystem::Stage::Game &&
            "MaterialSystem::UnregisterMaterial must run on the game stage");
        SpinLockGuard lock(m_Lock);

        if (index >= MAX_MATERIALS) return;

        m_Slots[index].material = nullptr;
        m_FreeIndices.push_back(index);
    }

    void MaterialSystem::Update(VkCommandBuffer cmd)
    {
        LH_PROFILE_FUNCTION();
        (void)cmd;
        assert(JobSystem::GetCurrentStage() == JobSystem::Stage::Game &&
            "MaterialSystem::Update must run on the game stage");

        // Allocate a fresh region for this frame; tag is the absolute frame index so FreeTag(N-2) reclaims it
        // once the GPU has retired the consuming frame.
        auto* jobCtx = JobSystem::GetCurrentJobContext();
        if (!jobCtx) return;
        const u64 gameFrame = Renderer::GetFrameData()->GetFrameIndex();
        jobCtx->GpuCache.CurrentTag = static_cast<u32>(gameFrame);

        constexpr u64 regionBytes = static_cast<u64>(MAX_MATERIALS) * MATERIAL_SIZE;
        constexpr u64 paramBytes  = static_cast<u64>(MAX_MATERIALS) * MAT_GRAPH_STRIDE * sizeof(Vec4);
        auto& heap = Memory::GPUTaggedPageAllocator::Get();
        Memory::GPUSubRegion region      = heap.Allocate(jobCtx->GpuCache, regionBytes, 16);
        Memory::GPUSubRegion paramRegion = heap.Allocate(jobCtx->GpuCache, paramBytes, 16);
        if (!region.buffer || !paramRegion.buffer) return;

        // Lock spans slot iteration (~25us at MAX_MATERIALS=16384). Borderline for V1 strictly, but contention is
        // zero today: Register/Unregister also assert game stage and serialize through RenderSnapshot::Capture.
        // Future parallel-register would justify shrinking to slot-alloc-only via an atomic free list.
        {
            SpinLockGuard lock(m_Lock);
            for (u32 i = 0; i < MAX_MATERIALS; ++i)
            {
                if (!m_Slots[i].material) continue;

                // Refresh GPU data each frame to pick up newly-loaded bindless texture indices.
                m_Slots[i].material->UpdateGPUData();
                const GPUMaterialData& data = m_Slots[i].material->GetGPUData();
                memcpy(static_cast<u8*>(region.mappedPtr) + (i * MATERIAL_SIZE), &data, MATERIAL_SIZE);

                // Graph constants fill the parallel param region at the same slot (paramBase = i*MAT_GRAPH_STRIDE).
                // The region is fresh each frame, so write every graph material; non-graph slots stay untouched.
                const auto& params = m_Slots[i].material->GetGraphParams();
                if (!params.empty())
                {
                    const u64 n = params.size() < MAT_GRAPH_STRIDE ? params.size() : MAT_GRAPH_STRIDE;
                    memcpy(static_cast<u8*>(paramRegion.mappedPtr) + (static_cast<u64>(i) * MAT_GRAPH_STRIDE * sizeof(Vec4)),
                           params.data(), n * sizeof(Vec4));
                }
                m_Slots[i].material->ClearGpuDirty();
            }
        }

        heap.FlushRegion(region);
        heap.FlushRegion(paramRegion);

        // Write the GAME-frame's slot (binding 0 = material SSBO, binding 1 = param buffer). Render stage K-1
        // reads slot (K-1)%N; distinct slots, race-free. Both written every frame (binding 1 never unbound).
        const u32 slot = static_cast<u32>(gameFrame) % MAX_FRAMES_IN_FLIGHT;

        VkDescriptorBufferInfo bi[2]{};
        bi[0].buffer = region.buffer;       bi[0].offset = region.offset;       bi[0].range = region.size;
        bi[1].buffer = paramRegion.buffer;  bi[1].offset = paramRegion.offset;  bi[1].range = paramRegion.size;

        VkWriteDescriptorSet writes[2]{};
        for (u32 b = 0; b < 2; ++b)
        {
            writes[b].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[b].dstSet          = m_DescriptorSets[slot];
            writes[b].dstBinding      = b;
            writes[b].dstArrayElement = 0;
            writes[b].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[b].descriptorCount = 1;
            writes[b].pBufferInfo     = &bi[b];
        }
        vkUpdateDescriptorSets(VulkanContext::Get().GetDevice(), 2, writes, 0, nullptr);
    }

    VkDescriptorSet MaterialSystem::GetDescriptorSet(u32 slot)
    {
        return m_DescriptorSets[slot % MAX_FRAMES_IN_FLIGHT];
    }

    VkDescriptorSetLayout MaterialSystem::GetDescriptorSetLayout()
    {
        return m_DescriptorSetLayout;
    }

    void MaterialSystem::CreateDescriptors()
    {
        LH_PROFILE_FUNCTION();
        VkDevice device = VulkanContext::Get().GetDevice();

        // binding 0 = material SSBO, binding 1 = graph-param buffer (gMatParams), both rewritten per game stage.
        // invariant: UAB even though slots cycle; the K%N write can fire while render-stage K's cmd buffer (same
        // slot) is still pending. The one set binds at Set 2 (raster) and Set 3 (RT megakernels).
        VkDescriptorSetLayoutBinding bindings[2]{};
        for (u32 b = 0; b < 2; ++b)
        {
            bindings[b].binding         = b;
            bindings[b].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            bindings[b].descriptorCount = 1;
            bindings[b].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;
        }

        VkDescriptorBindingFlags bindingFlags[2] = {
            VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT, VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT };
        VkDescriptorSetLayoutBindingFlagsCreateInfo bindingFlagsCI{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO };
        bindingFlagsCI.bindingCount  = 2;
        bindingFlagsCI.pBindingFlags = bindingFlags;

        VkDescriptorSetLayoutCreateInfo layoutInfo{};
        layoutInfo.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layoutInfo.pNext        = &bindingFlagsCI;
        layoutInfo.flags        = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
        layoutInfo.bindingCount = 2;
        layoutInfo.pBindings    = bindings;
        vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &m_DescriptorSetLayout);

        // Pool sized for 2 storage buffers per set (material SSBO + param buffer) across N slots.
        VkDescriptorPoolSize poolSize{};
        poolSize.type            = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        poolSize.descriptorCount = 2 * MAX_FRAMES_IN_FLIGHT;

        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.flags         = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
        poolInfo.maxSets       = MAX_FRAMES_IN_FLIGHT;
        poolInfo.poolSizeCount = 1;
        poolInfo.pPoolSizes    = &poolSize;
        vkCreateDescriptorPool(device, &poolInfo, nullptr, &m_DescriptorPool);

        // Allocate all N slots.
        std::array<VkDescriptorSetLayout, MAX_FRAMES_IN_FLIGHT> layouts;
        layouts.fill(m_DescriptorSetLayout);

        VkDescriptorSetAllocateInfo allocInfo{};
        allocInfo.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocInfo.descriptorPool     = m_DescriptorPool;
        allocInfo.descriptorSetCount = MAX_FRAMES_IN_FLIGHT;
        allocInfo.pSetLayouts        = layouts.data();
        vkAllocateDescriptorSets(device, &allocInfo, m_DescriptorSets.data());
        for (u32 i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i)
        {
            char name[48]; std::snprintf(name, sizeof(name), "Material.Slot%u", i);
            VulkanContext::SetDebugName(m_DescriptorSets[i], name);
        }
    }
}
