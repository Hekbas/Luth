#include "luthpch.h"
#include "MaterialSystem.h"
#include "luth/renderer/Renderer.h"
#include "luth/renderer/backend/vulkan/VulkanContext.h"
#include "luth/core/FrameData.h"
#include "luth/core/diagnostics/Log.h"
#include "luth/jobs/JobSystem.h"
#include "luth/memory/GPUTaggedPageAllocator.h"

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

        LH_CORE_INFO("Material System Initialized (Max Materials: {0})", MAX_MATERIALS);
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
        // Slot mutation must run on the game stage; concurrent Render(N-1)
        // reads the slot map without locking.
        assert(JobSystem::GetCurrentStage() == JobSystem::Stage::Game &&
            "MaterialSystem::RegisterMaterial must run on the game stage");
        SpinLockGuard lock(m_Lock);

        if (m_FreeIndices.empty())
        {
            LH_CORE_ERROR("Material System: Out of slots!");
            return 0;
        }

        u32 index = m_FreeIndices.front();
        m_FreeIndices.pop_front();
        m_Slots[index].material = material;
        return index;
    }

    void MaterialSystem::UnregisterMaterial(u32 index)
    {
        assert(JobSystem::GetCurrentStage() == JobSystem::Stage::Game &&
            "MaterialSystem::UnregisterMaterial must run on the game stage");
        SpinLockGuard lock(m_Lock);

        if (index >= MAX_MATERIALS) return;

        m_Slots[index].material = nullptr;
        m_FreeIndices.push_back(index);
    }

    void MaterialSystem::Update(VkCommandBuffer cmd)
    {
        (void)cmd;
        assert(JobSystem::GetCurrentStage() == JobSystem::Stage::Game &&
            "MaterialSystem::Update must run on the game stage");

        // Allocate a fresh region for this frame; tag is the absolute frame index so
        // FreeTag(N-2) reclaims it once the GPU has retired the consuming frame.
        auto* jobCtx = JobSystem::GetCurrentJobContext();
        if (!jobCtx) return;
        const u64 gameFrame = Renderer::GetFrameData()->GetFrameIndex();
        jobCtx->GpuCache.CurrentTag = static_cast<u32>(gameFrame);

        constexpr u64 regionBytes = static_cast<u64>(MAX_MATERIALS) * MATERIAL_SIZE;
        auto& heap = Memory::GPUTaggedPageAllocator::Get();
        Memory::GPUSubRegion region = heap.Allocate(jobCtx->GpuCache, regionBytes, 16);
        if (!region.buffer) return;

        // Lock spans slot iteration (~25us at MAX_MATERIALS=16384). Borderline for V1
        // strictly, but contention is zero today: Register/Unregister also assert game
        // stage and serialize through RenderSnapshot::Capture. Future parallel-register
        // would justify shrinking to slot-alloc-only via an atomic free list.
        {
            SpinLockGuard lock(m_Lock);
            for (u32 i = 0; i < MAX_MATERIALS; ++i)
            {
                if (!m_Slots[i].material) continue;

                // Refresh GPU data each frame to pick up newly-loaded bindless texture indices.
                m_Slots[i].material->UpdateGPUData();
                const GPUMaterialData& data = m_Slots[i].material->GetGPUData();
                u8* dst = static_cast<u8*>(region.mappedPtr) + (i * MATERIAL_SIZE);
                memcpy(dst, &data, MATERIAL_SIZE);
                m_Slots[i].material->ClearGpuDirty();
            }
        }

        heap.FlushRegion(region);

        // Write the GAME-frame's Set 2 slot. Render stage K-1 reads slot (K-1)%N
        // — distinct slots, race-free, no UAB needed.
        const u32 slot = static_cast<u32>(gameFrame) % MAX_FRAMES_IN_FLIGHT;

        VkDescriptorBufferInfo bi{};
        bi.buffer = region.buffer;
        bi.offset = region.offset;
        bi.range  = region.size;

        VkWriteDescriptorSet write{};
        write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet          = m_DescriptorSets[slot];
        write.dstBinding      = 0;
        write.dstArrayElement = 0;
        write.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        write.descriptorCount = 1;
        write.pBufferInfo     = &bi;
        vkUpdateDescriptorSets(VulkanContext::Get().GetDevice(), 1, &write, 0, nullptr);
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
        VkDevice device = VulkanContext::Get().GetDevice();

        // invariant: Set 2 needs UAB even though slots are cycled. The K%N write at
        // game-stage K can fire while render-stage K's cmd buffer (which binds the
        // same slot, recording in parallel) is still in the pending state.
        VkDescriptorSetLayoutBinding binding{};
        binding.binding         = 0;
        binding.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        binding.descriptorCount = 1;
        binding.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;

        VkDescriptorBindingFlags bindingFlags = VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;
        VkDescriptorSetLayoutBindingFlagsCreateInfo bindingFlagsCI{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO };
        bindingFlagsCI.bindingCount  = 1;
        bindingFlagsCI.pBindingFlags = &bindingFlags;

        VkDescriptorSetLayoutCreateInfo layoutInfo{};
        layoutInfo.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layoutInfo.pNext        = &bindingFlagsCI;
        layoutInfo.flags        = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
        layoutInfo.bindingCount = 1;
        layoutInfo.pBindings    = &binding;
        vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &m_DescriptorSetLayout);

        // Pool sized for MAX_FRAMES_IN_FLIGHT sets; UPDATE_AFTER_BIND pairs the layout flag.
        VkDescriptorPoolSize poolSize{};
        poolSize.type            = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        poolSize.descriptorCount = MAX_FRAMES_IN_FLIGHT;

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
