#include "luthpch.h"
#include "luth/renderer/RenderPipeline.h"
#include "luth/core/FrameData.h"
#include "luth/core/RenderSnapshot.h"
#include "luth/scene/systems/RenderingSystem.h"
#include "luth/renderer/Renderer.h"
#include "luth/renderer/backend/vulkan/VulkanContext.h"
#include "luth/renderer/backend/vulkan/VulkanBuffer.h"
#include "luth/renderer/backend/vulkan/VulkanComputePipeline.h"
#include "luth/renderer/material/MaterialSystem.h"
#include "luth/renderer/material/Material.h"
#include "luth/renderer/resources/Model.h"
#include "luth/renderer/resources/Buffer.h"
#include "luth/renderer/draw/DrawCommand.h"
#include "luth/renderer/shader/ShaderLibrary.h"
#include "luth/resources/AssetManager.h"
#include "luth/jobs/JobSystem.h"
#include "luth/memory/GPUTaggedPageAllocator.h"

namespace Luth
{

    void RenderPipeline::InitObjectSSBODescriptorLayout()
    {
        if (m_ObjectSSBODescLayout != VK_NULL_HANDLE)
            return;

        VkDevice device = VulkanContext::Get().GetDevice();

        VkDescriptorSetLayoutBinding binding{};
        binding.binding         = 0;
        binding.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        binding.descriptorCount = 1;
        binding.stageFlags      = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

        // UPDATE_AFTER_BIND so BuildGPUObjectBuffer can rewrite the binding each frame.
        VkDescriptorBindingFlags bindingFlags = VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;
        VkDescriptorSetLayoutBindingFlagsCreateInfo bindingFlagsInfo{};
        bindingFlagsInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
        bindingFlagsInfo.bindingCount = 1;
        bindingFlagsInfo.pBindingFlags = &bindingFlags;

        VkDescriptorSetLayoutCreateInfo layoutInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        layoutInfo.pNext        = &bindingFlagsInfo;
        layoutInfo.flags        = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
        layoutInfo.bindingCount = 1;
        layoutInfo.pBindings    = &binding;

        vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &m_ObjectSSBODescLayout);
    }

    void RenderPipeline::InitGPUObjectBuffers()
    {
        // Set 5 descriptor pool + set for the per-frame ObjectSSBO region.
        // No persistent VkBuffer — BuildGPUObjectBuffer allocates from the GPU
        // tagged heap each frame and rewrites this descriptor.
        VkDevice device = VulkanContext::Get().GetDevice();

        VkDescriptorPoolSize poolSize{};
        poolSize.type            = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        poolSize.descriptorCount = 1;

        VkDescriptorPoolCreateInfo poolInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        poolInfo.flags         = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
        poolInfo.maxSets       = 1;
        poolInfo.poolSizeCount = 1;
        poolInfo.pPoolSizes    = &poolSize;
        vkCreateDescriptorPool(device, &poolInfo, nullptr, &m_ObjectSSBODescPool);

        VkDescriptorSetAllocateInfo allocInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        allocInfo.descriptorPool     = m_ObjectSSBODescPool;
        allocInfo.descriptorSetCount = 1;
        allocInfo.pSetLayouts        = &m_ObjectSSBODescLayout;
        vkAllocateDescriptorSets(device, &allocInfo, &m_ObjectSSBODescSet);
    }

    void RenderPipeline::InitCullPipeline()
    {
        VkDevice device = VulkanContext::Get().GetDevice();

        // Descriptor layout: binding 0 = ObjectSSBO (read), binding 1 = IndirectBuffer (write).
        // Both bindings get UPDATE_AFTER_BIND so BuildGPUObjectBuffer can rewrite each frame.
        VkDescriptorSetLayoutBinding bindings[2]{};
        bindings[0].binding         = 0;
        bindings[0].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[0].descriptorCount = 1;
        bindings[0].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
        bindings[1].binding         = 1;
        bindings[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[1].descriptorCount = 1;
        bindings[1].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;

        VkDescriptorBindingFlags bindingFlags[2] = {
            VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT,
            VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT,
        };
        VkDescriptorSetLayoutBindingFlagsCreateInfo bindingFlagsInfo{};
        bindingFlagsInfo.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
        bindingFlagsInfo.bindingCount  = 2;
        bindingFlagsInfo.pBindingFlags = bindingFlags;

        VkDescriptorSetLayoutCreateInfo layoutInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        layoutInfo.pNext        = &bindingFlagsInfo;
        layoutInfo.flags        = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
        layoutInfo.bindingCount = 2;
        layoutInfo.pBindings    = bindings;
        vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &m_CullDescLayout);

        // Dedicated pool: shared DescriptorAllocator's pool isn't UPDATE_AFTER_BIND-capable.
        VkDescriptorPoolSize cullPoolSize{};
        cullPoolSize.type            = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        cullPoolSize.descriptorCount = 2;

        VkDescriptorPoolCreateInfo cullPoolInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        cullPoolInfo.flags         = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
        cullPoolInfo.maxSets       = 1;
        cullPoolInfo.poolSizeCount = 1;
        cullPoolInfo.pPoolSizes    = &cullPoolSize;
        vkCreateDescriptorPool(device, &cullPoolInfo, nullptr, &m_CullDescPool);

        VkDescriptorSetAllocateInfo cullAllocInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        cullAllocInfo.descriptorPool     = m_CullDescPool;
        cullAllocInfo.descriptorSetCount = 1;
        cullAllocInfo.pSetLayouts        = &m_CullDescLayout;
        vkAllocateDescriptorSets(device, &cullAllocInfo, &m_CullDescSet);

        // Push-constant range: 6 frustum planes (96B) + objectCount + destOffset = 104B.
        // srcOffset deleted under gpu-tagged-heap (cull reads objects[idx], 0-based).
        VkPushConstantRange pcRange{};
        pcRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        pcRange.offset     = 0;
        pcRange.size       = sizeof(Vec4) * 6 + sizeof(u32) * 2;

        auto cullShader = ShaderLibrary::LoadEngine("shaders/gpu_cull.comp");
        auto spv = cullShader ? cullShader->GetSpirV() : std::vector<u32>{};
        if (spv.empty())
        {
            LH_CORE_ERROR("RenderingSystem: Failed to load gpu_cull.comp!");
            return;
        }

        m_CullPipeline = std::make_unique<VKComputePipeline>(
            spv,
            std::vector<VkDescriptorSetLayout>{ m_CullDescLayout },
            std::vector<VkPushConstantRange>{ pcRange });
    }

    u32 RenderPipeline::EnsureMaterialRegistered(std::shared_ptr<Material> material)
    {
        auto it = m_MaterialSlotMap.find(material->Handle);
        if (it != m_MaterialSlotMap.end())
            return it->second;

        u32 slot = MaterialSystem::RegisterMaterial(material);
        m_MaterialSlotMap[material->Handle] = slot;
        return slot;
    }

    void RenderPipeline::BuildGPUObjectBuffer(const RenderSnapshot& snapshot)
    {
        // Allocate fresh regions from the GPU tagged heap. Tag is the absolute
        // render-frame index so FreeTag(N-2) reclaims them once the GPU retires.
        auto* jobCtx = JobSystem::GetCurrentJobContext();
        if (!jobCtx) return;
        jobCtx->GpuCache.CurrentTag = static_cast<u32>(Renderer::GetFrameData()->GetRenderFrameIndex());

        auto& heap = Memory::GPUTaggedPageAllocator::Get();
        const u64 objBytes = static_cast<u64>(RenderPipeline::k_MaxGPUObjects) * sizeof(GPUObjectData);
        const u64 indBytes = static_cast<u64>(RenderPipeline::k_IndirectRegionCount)
                           * RenderPipeline::k_IndirectRegionStride
                           * sizeof(VkDrawIndexedIndirectCommand);

        m_ObjectRegion   = heap.Allocate(jobCtx->GpuCache, objBytes, 16);
        m_IndirectRegion = heap.Allocate(jobCtx->GpuCache, indBytes, 16);
        if (!m_ObjectRegion.buffer || !m_IndirectRegion.buffer) { m_GPUObjectCount = 0; return; }

        auto* objectData   = static_cast<GPUObjectData*>(m_ObjectRegion.mappedPtr);
        auto* indirectCmds = static_cast<VkDrawIndexedIndirectCommand*>(m_IndirectRegion.mappedPtr);
        u32   count        = 0;

        // Rebuild entity lookup table here (consumed by GeometryPass + mouse picking).
        // index 0 = null sentinel; valid entities start at index 1.
        m_EntityLookup.clear();
        m_EntityLookup.push_back(entt::null);
        m_EntityToSSBOIndex.clear();

        for (const MeshDrawSnapshot& meshSnap : snapshot.meshes)
        {
            if (count >= RenderPipeline::k_MaxGPUObjects) break;

            // Snapshot already filtered out invalid model + OOB mesh index, but
            // GetMesh + GetIndexBuffer still need the model lookup for AABB and IB.
            auto model = AssetManager::GetAsset<Model>(meshSnap.modelUUID);
            if (!model) continue;
            const auto& meshesData = model->GetMeshesData();
            auto mesh = model->GetMesh(meshSnap.meshIndex);
            if (!mesh) continue;

            GPUObjectData& obj = objectData[count];
            obj.model = meshSnap.worldMatrix;

            // Bounding sphere from BindPoseAABB (local space)
            const auto& aabb   = meshesData[meshSnap.meshIndex].BindPoseAABB;
            obj.boundingSphere = Vec4(aabb.Center(), Math::Length(aabb.Extents()));

            // Material slot — 0-based. Set 2 is rebound per game stage to the active
            // frame's tagged-heap region (see MaterialSystem::Update); the shader's
            // materials[obj.materialIndex] indexes into that region directly.
            u32 matSlot = 0;
            if (meshSnap.materialUUID.IsValid()) {
                auto it = m_MaterialSlotMap.find(meshSnap.materialUUID);
                if (it != m_MaterialSlotMap.end()) matSlot = it->second;
            }
            obj.materialIndex = matSlot;
            obj.shadeMode     = static_cast<u32>(m_System.m_ShadeMode);
            // entityID is 1-indexed so the fragment shader output matches m_EntityLookup
            obj.entityID      = (u32)m_EntityLookup.size();
            obj.boneOffset    = meshSnap.boneOffset;

            entt::entity entity = static_cast<entt::entity>(meshSnap.entity);
            m_EntityLookup.push_back(entity);
            m_EntityToSSBOIndex[entity] = count;

            auto* ib = std::static_pointer_cast<VKIndexBuffer>(mesh->GetIndexBuffer()).get();
            obj.indexCount   = ib ? ib->GetCount() : 0;
            obj.firstIndex   = 0;
            obj.vertexOffset = 0;
            obj._pad         = 0;

            // Indirect command — instanceCount=1; per-region GPU cull zeros it if culled.
            // firstInstance = 0-based count: gl_BaseInstance in shader → objects[gl_BaseInstance]
            // indexes the active region (region binding starts at m_ObjectRegion.offset).
            // Duplicate into all RenderPipeline::k_IndirectRegionCount regions
            // (camera + 4 cascades × all views) so each region has its own
            // independently-cullable command for this object.
            VkDrawIndexedIndirectCommand baseCmd{};
            baseCmd.indexCount    = obj.indexCount;
            baseCmd.instanceCount = 1;
            baseCmd.firstIndex    = 0;
            baseCmd.vertexOffset  = 0;
            baseCmd.firstInstance = count;
            for (u32 r = 0; r < RenderPipeline::k_IndirectRegionCount; ++r)
                indirectCmds[r * RenderPipeline::k_IndirectRegionStride + count] = baseCmd;
            count++;
        }

        m_GPUObjectCount = count;

        heap.FlushRegion(m_ObjectRegion);
        heap.FlushRegion(m_IndirectRegion);

        // Rewrite Set 5 (graphics) + cull descriptors to point at this frame's regions.
        // Both layouts use UPDATE_AFTER_BIND_BIT so updates are safe under in-flight cmd buffers.
        VkDevice device = VulkanContext::Get().GetDevice();
        {
            VkDescriptorBufferInfo bi{};
            bi.buffer = m_ObjectRegion.buffer;
            bi.offset = m_ObjectRegion.offset;
            bi.range  = m_ObjectRegion.size;

            VkWriteDescriptorSet write{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            write.dstSet          = m_ObjectSSBODescSet;
            write.dstBinding      = 0;
            write.descriptorCount = 1;
            write.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            write.pBufferInfo     = &bi;
            vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
        }
        {
            VkDescriptorBufferInfo objInfo{};
            objInfo.buffer = m_ObjectRegion.buffer;
            objInfo.offset = m_ObjectRegion.offset;
            objInfo.range  = m_ObjectRegion.size;

            VkDescriptorBufferInfo indInfo{};
            indInfo.buffer = m_IndirectRegion.buffer;
            indInfo.offset = m_IndirectRegion.offset;
            indInfo.range  = m_IndirectRegion.size;

            VkWriteDescriptorSet writes[2]{};
            writes[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[0].dstSet          = m_CullDescSet;
            writes[0].dstBinding      = 0;
            writes[0].descriptorCount = 1;
            writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[0].pBufferInfo     = &objInfo;
            writes[1]                 = writes[0];
            writes[1].dstBinding      = 1;
            writes[1].pBufferInfo     = &indInfo;
            vkUpdateDescriptorSets(device, 2, writes, 0, nullptr);
        }
    }

    void RenderPipeline::UploadLightUBO(const LightUniforms& lights)
    {
        m_LightUniformBuffer->SetData(&lights, sizeof(LightUniforms));
    }
}
