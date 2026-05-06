#include "lepch.h"
#include "luthien/widgets/ThumbnailPreviewScene.h"

#include "luth/core/diagnostics/Log.h"
#include "luth/core/types/LuthMath.h"
#include "luth/renderer/Renderer.h"
#include "luth/renderer/RenderBackend.h"
#include "luth/renderer/backend/vulkan/VulkanAllocator.h"
#include "luth/renderer/backend/vulkan/VulkanBuffer.h"
#include "luth/renderer/backend/vulkan/TimelineSemaphore.h"
#include "luth/renderer/backend/vulkan/VulkanContext.h"
#include "luth/renderer/backend/vulkan/VulkanPipeline.h"
#include "luth/renderer/backend/vulkan/VulkanTexture.h"
#include "luth/renderer/material/Material.h"
#include "luth/renderer/resources/Mesh.h"
#include "luth/renderer/resources/Model.h"
#include "luth/renderer/resources/Texture.h"
#include "luth/renderer/shader/ShaderLibrary.h"
#include "luth/renderer/shader/Shader.h"
#include "luth/resources/AssetManager.h"

#include <backends/imgui_impl_vulkan.h>
#include <vma/vk_mem_alloc.h>

#include <array>
#include <cmath>
#include <cstring>
#include <memory>

namespace Luth::UI::ThumbnailPreviewScene
{
    namespace
    {
        constexpr u32 kSize                = 128;
        constexpr u32 kStaging             = kSize * kSize * 4;       // RGBA8
        constexpr u32 kStaticVertexStride  = sizeof(Vertex);          // 52 B
        constexpr u32 kSkinnedVertexStride = sizeof(SkinnedVertex);   // 84 B

        struct PushConstants
        {
            Mat4 viewProj;
            Vec4 albedo;
        };
        static_assert(sizeof(PushConstants) == 80, "thumbnail PC layout drift");

        // Two pipelines share the same shaders (Position@0 + Normal@12 + UV@24
        // are at identical offsets in Vertex and SkinnedVertex); only the
        // binding stride differs. Skinned bakes render bind pose.
        bool                        s_Initialized = false;
        std::unique_ptr<VKPipeline> s_PipelineStatic;
        std::unique_ptr<VKPipeline> s_PipelineSkinned;
        std::shared_ptr<Texture>    s_ColorRT;
        std::shared_ptr<Texture>    s_DepthRT;
        VkBuffer                    s_Staging       = VK_NULL_HANDLE;
        VmaAllocation               s_StagingAlloc  = nullptr;
        void*                       s_StagingMapped = nullptr;

        // Per-bake descriptor: set=0 binding=0 = combined image sampler. Updated
        // each bake to point at the chosen texture (white for mesh, material's
        // albedo otherwise). invariant: ImmediateSubmit waits before returning,
        // so the descriptor is never in flight across updates.
        VkDescriptorSetLayout       s_SamplerLayout = VK_NULL_HANDLE;
        VkDescriptorPool            s_SamplerPool   = VK_NULL_HANDLE;
        VkDescriptorSet             s_SamplerSet    = VK_NULL_HANDLE;

        // 1x1 white default texture — bound for mesh bakes (no albedo texture)
        // and as a fallback when a material has no albedo map. Created via
        // Texture::Create (async upload) then flushed with vkDeviceWaitIdle.
        std::shared_ptr<Texture>    s_WhiteTexture;

        // Sphere primitive used for material bakes. Lazy-loaded on first
        // BakeMaterial call to avoid the upfront cost when no project / no
        // material thumbnails are needed yet.
        std::shared_ptr<Model>      s_SphereModel;

        // Async ring submission: per-slot RT + sampler set + cmd buffer signaled
        // via dedicated timeline semaphore so per-frame inspector renders don't
        // stall the device. Lazy-init on first inspector call. Slot count matches
        // MAX_FRAMES_IN_FLIGHT (game can be 3 frames ahead of GPU).
        constexpr u32 kInspectorSize     = 256;
        constexpr u32 kInspectorRingSize = 3; // matches MAX_FRAMES_IN_FLIGHT

        struct InspectorSlot
        {
            std::shared_ptr<Texture> color;
            std::shared_ptr<Texture> depth;
            VkDescriptorSet          imguiSet     = VK_NULL_HANDLE;
            VkDescriptorSet          samplerSet   = VK_NULL_HANDLE;
            VkCommandBuffer          cmd          = VK_NULL_HANDLE;
            u64                      timelineValue = 0;
        };

        std::array<InspectorSlot, kInspectorRingSize> s_Ring{};
        VkCommandPool         s_InspectorCmdPool        = VK_NULL_HANDLE;
        VkDescriptorSetLayout s_InspectorSamplerLayout  = VK_NULL_HANDLE;
        VkDescriptorPool      s_InspectorSamplerPool    = VK_NULL_HANDLE;
        TimelineSemaphore     s_InspectorTimeline;
        u64                   s_NextSubmitValue         = 0;
        u32                   s_RingHead                = 0;
        ImTextureID           s_LastGoodInspectorTex    = (ImTextureID)0;

        // Material-change gate keys — `RenderMaterialInspector` re-runs
        // LoadImmediate + vkDeviceWaitIdle only when the inspected material's
        // identity or its texture-map UUID set changes.
        UUID                  s_LastInspectorMaterialHandle = UUID::Invalid();
        size_t                s_LastInspectorMaterialTexHash = 0;

        bool VulkanActive()
        {
            return Renderer::GetBackend()
                && Renderer::GetBackend()->GetAPI() == RenderBackend::API::Vulkan;
        }

        bool LoadShader(const char* relPath, std::vector<u32>& out)
        {
            auto sh = ShaderLibrary::LoadEngine(relPath);
            if (!sh) { LH_CORE_ERROR("Thumbnail: failed to load engine shader '{}'", relPath); return false; }
            out = sh->GetSpirV();
            return !out.empty();
        }

        std::unique_ptr<VKPipeline> BuildPipeline(const std::vector<u32>& vert,
                                                  const std::vector<u32>& frag,
                                                  u32 stride)
        {
            PipelineConfig cfg;
            cfg.colorFormats   = { VK_FORMAT_R8G8B8A8_UNORM };
            cfg.depthFormat    = VK_FORMAT_D32_SFLOAT;
            cfg.depthTest      = true;
            cfg.depthWrite     = true;
            cfg.depthCompareOp = VK_COMPARE_OP_LESS;
            cfg.blendEnabled   = false;
            cfg.cullMode       = VK_CULL_MODE_BACK_BIT;
            cfg.frontFace      = VK_FRONT_FACE_COUNTER_CLOCKWISE;
            cfg.topology       = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            cfg.polygonMode    = VK_POLYGON_MODE_FILL;

            VkVertexInputBindingDescription bind{};
            bind.binding   = 0;
            bind.stride    = stride;
            bind.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
            cfg.bindingDescriptions = { bind };

            // Position@0, Normal@12, TexCoord0@24 are at identical offsets in
            // both Vertex and SkinnedVertex; only the per-vertex stride differs.
            VkVertexInputAttributeDescription pos{};
            pos.location = 0;
            pos.binding  = 0;
            pos.format   = VK_FORMAT_R32G32B32_SFLOAT;
            pos.offset   = offsetof(Vertex, Position);

            VkVertexInputAttributeDescription nrm{};
            nrm.location = 1;
            nrm.binding  = 0;
            nrm.format   = VK_FORMAT_R32G32B32_SFLOAT;
            nrm.offset   = offsetof(Vertex, Normal);

            VkVertexInputAttributeDescription uv{};
            uv.location = 2;
            uv.binding  = 0;
            uv.format   = VK_FORMAT_R32G32_SFLOAT;
            uv.offset   = offsetof(Vertex, TexCoord0);

            cfg.attributeDescriptions = { pos, nrm, uv };

            VkPushConstantRange pc{};
            pc.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
            pc.offset     = 0;
            pc.size       = sizeof(PushConstants);
            cfg.pushConstantRanges = { pc };

            return std::make_unique<VKPipeline>(cfg, vert, frag,
                                                std::vector<VkDescriptorSetLayout>{ s_SamplerLayout });
        }

        bool CreateSamplerDescriptor()
        {
            VkDevice device = VulkanContext::Get().GetDevice();

            VkDescriptorSetLayoutBinding binding{};
            binding.binding         = 0;
            binding.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            binding.descriptorCount = 1;
            binding.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

            VkDescriptorSetLayoutCreateInfo layoutInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
            layoutInfo.bindingCount = 1;
            layoutInfo.pBindings    = &binding;
            if (vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &s_SamplerLayout) != VK_SUCCESS)
                return false;

            VkDescriptorPoolSize poolSize{};
            poolSize.type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            poolSize.descriptorCount = 1;

            VkDescriptorPoolCreateInfo poolInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
            poolInfo.maxSets       = 1;
            poolInfo.poolSizeCount = 1;
            poolInfo.pPoolSizes    = &poolSize;
            if (vkCreateDescriptorPool(device, &poolInfo, nullptr, &s_SamplerPool) != VK_SUCCESS)
                return false;

            VkDescriptorSetAllocateInfo alloc{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            alloc.descriptorPool     = s_SamplerPool;
            alloc.descriptorSetCount = 1;
            alloc.pSetLayouts        = &s_SamplerLayout;
            return vkAllocateDescriptorSets(device, &alloc, &s_SamplerSet) == VK_SUCCESS;
        }

        bool CreateWhiteTexture()
        {
            // Async-upload path; flush with vkDeviceWaitIdle below so the
            // first sample isn't UB. Init runs once at editor startup so the
            // wait has no per-frame cost.
            const u8 white[4] = { 255, 255, 255, 255 };
            s_WhiteTexture = Texture::Create(1, 1, TextureFormat::RGBA8, white);
            if (!s_WhiteTexture) return false;
            vkDeviceWaitIdle(VulkanContext::Get().GetDevice());
            return true;
        }

        void UpdateSamplerDescriptor(VkImageView view, VkSampler sampler)
        {
            VkDescriptorImageInfo info{};
            info.sampler     = sampler;
            info.imageView   = view;
            info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

            VkWriteDescriptorSet w{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            w.dstSet          = s_SamplerSet;
            w.dstBinding      = 0;
            w.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            w.descriptorCount = 1;
            w.pImageInfo      = &info;
            vkUpdateDescriptorSets(VulkanContext::Get().GetDevice(), 1, &w, 0, nullptr);
        }

        // Per-slot variant for the async inspector path. The slot's sampler set
        // is private to its in-flight cmd buffer, so writes here cannot race
        // a pending submission as long as the caller has waited on that slot's
        // timeline value before recording.
        void UpdateInspectorSamplerSet(VkDescriptorSet set, VkImageView view, VkSampler sampler)
        {
            VkDescriptorImageInfo info{};
            info.sampler     = sampler;
            info.imageView   = view;
            info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

            VkWriteDescriptorSet w{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            w.dstSet          = set;
            w.dstBinding      = 0;
            w.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            w.descriptorCount = 1;
            w.pImageInfo      = &info;
            vkUpdateDescriptorSets(VulkanContext::Get().GetDevice(), 1, &w, 0, nullptr);
        }

        bool CreatePipelines()
        {
            std::vector<u32> vert, frag;
            if (!LoadShader("shaders/thumbnail_mesh.vert", vert)) return false;
            if (!LoadShader("shaders/thumbnail_mesh.frag", frag)) return false;

            s_PipelineStatic  = BuildPipeline(vert, frag, kStaticVertexStride);
            s_PipelineSkinned = BuildPipeline(vert, frag, kSkinnedVertexStride);
            return s_PipelineStatic && s_PipelineSkinned;
        }

        bool CreateTargets()
        {
            s_ColorRT = Texture::Create(kSize, kSize, TextureFormat::RGBA8);
            s_DepthRT = Texture::Create(kSize, kSize, TextureFormat::D32_Float);
            return s_ColorRT && s_DepthRT;
        }

        bool CreateStaging()
        {
            VkBufferCreateInfo bi{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
            bi.size  = kStaging;
            bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            s_StagingAlloc = VulkanAllocator::AllocateBuffer(bi, VMA_MEMORY_USAGE_CPU_ONLY, s_Staging);
            if (!s_StagingAlloc) return false;
            s_StagingMapped = VulkanAllocator::Map(s_StagingAlloc);
            return s_StagingMapped != nullptr;
        }

        // Issues a one-shot image memory barrier in the cmd buffer.
        void Barrier(VkCommandBuffer cmd, VkImage image, VkImageAspectFlags aspect,
                     VkImageLayout from, VkImageLayout to,
                     VkAccessFlags srcA, VkAccessFlags dstA,
                     VkPipelineStageFlags srcS, VkPipelineStageFlags dstS)
        {
            VkImageMemoryBarrier b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
            b.oldLayout                       = from;
            b.newLayout                       = to;
            b.srcQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
            b.dstQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
            b.image                           = image;
            b.subresourceRange.aspectMask     = aspect;
            b.subresourceRange.baseMipLevel   = 0;
            b.subresourceRange.levelCount     = 1;
            b.subresourceRange.baseArrayLayer = 0;
            b.subresourceRange.layerCount     = 1;
            b.srcAccessMask                   = srcA;
            b.dstAccessMask                   = dstA;
            vkCmdPipelineBarrier(cmd, srcS, dstS, 0, 0, nullptr, 0, nullptr, 1, &b);
        }
    }

    bool Init()
    {
        if (s_Initialized) return true;
        if (!VulkanActive()) return false;

        // Sampler layout/pool/set first — pipelines depend on the layout.
        if (!CreateSamplerDescriptor()) { Shutdown(); return false; }
        if (!CreatePipelines())         { Shutdown(); return false; }
        if (!CreateTargets())           { Shutdown(); return false; }
        if (!CreateStaging())           { Shutdown(); return false; }
        if (!CreateWhiteTexture())      { Shutdown(); return false; }

        s_Initialized = true;
        return true;
    }

    void Shutdown()
    {
        s_Initialized = false;

        // Drain in-flight inspector submissions before tearing down their
        // resources. Timeline must reach the last signal value or we may
        // free a cmd buffer / RT the GPU is still consuming.
        VkDevice device = VulkanActive() ? VulkanContext::Get().GetDevice() : VK_NULL_HANDLE;
        if (device != VK_NULL_HANDLE
            && s_InspectorTimeline.GetHandle() != VK_NULL_HANDLE
            && s_NextSubmitValue > 0)
        {
            s_InspectorTimeline.Wait(s_NextSubmitValue);
        }

        s_PipelineStatic.reset();   // ~VKPipeline destroys VkPipeline + layout
        s_PipelineSkinned.reset();
        s_ColorRT.reset();          // ~VKTexture pushes image deletion to fenced queue
        s_DepthRT.reset();

        // Inspector ring teardown. ImGui descriptor pool is destroyed during
        // Editor::Shutdown alongside ours; skip ImGui_ImplVulkan_RemoveTexture
        // (would race with pool destroy).
        for (auto& slot : s_Ring) {
            slot.color.reset();
            slot.depth.reset();
            slot.imguiSet     = VK_NULL_HANDLE;
            slot.samplerSet   = VK_NULL_HANDLE;
            slot.cmd          = VK_NULL_HANDLE;
            slot.timelineValue = 0;
        }
        s_LastGoodInspectorTex         = (ImTextureID)0;
        s_LastInspectorMaterialHandle  = UUID::Invalid();
        s_LastInspectorMaterialTexHash = 0;
        s_RingHead                     = 0;
        s_NextSubmitValue              = 0;

        s_WhiteTexture.reset();
        s_SphereModel.reset();

        if (device != VK_NULL_HANDLE) {
            // Pool destroy frees the allocated sets and command buffers.
            if (s_InspectorSamplerPool   != VK_NULL_HANDLE) vkDestroyDescriptorPool(device, s_InspectorSamplerPool, nullptr);
            if (s_InspectorSamplerLayout != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(device, s_InspectorSamplerLayout, nullptr);
            if (s_InspectorCmdPool       != VK_NULL_HANDLE) vkDestroyCommandPool(device, s_InspectorCmdPool, nullptr);

            if (s_SamplerPool   != VK_NULL_HANDLE) vkDestroyDescriptorPool(device, s_SamplerPool, nullptr);
            if (s_SamplerLayout != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(device, s_SamplerLayout, nullptr);
        }
        s_InspectorSamplerPool   = VK_NULL_HANDLE;
        s_InspectorSamplerLayout = VK_NULL_HANDLE;
        s_InspectorCmdPool       = VK_NULL_HANDLE;
        s_InspectorTimeline.Shutdown();

        s_SamplerPool   = VK_NULL_HANDLE;
        s_SamplerLayout = VK_NULL_HANDLE;
        s_SamplerSet    = VK_NULL_HANDLE;

        if (s_StagingAlloc) {
            if (s_StagingMapped) VulkanAllocator::Unmap(s_StagingAlloc);
            VulkanAllocator::FreeBuffer(s_Staging, s_StagingAlloc);
            s_Staging       = VK_NULL_HANDLE;
            s_StagingAlloc  = nullptr;
            s_StagingMapped = nullptr;
        }
    }

    u32 GetSize() { return kSize; }

    namespace
    {
        // Sphere primitive UUID — luth/assets/models/primitives/Sphere.fbx.
        // Stable across builds via the .meta file shipped with the engine.
        const UUID kSphereUUID = UUID::FromString("4c5ad301-7125-475e-954d-80c64c38a552");

        bool LoadSphereLazy()
        {
            if (s_SphereModel) return true;
            auto base = AssetManager::LoadImmediate(kSphereUUID);
            s_SphereModel = std::dynamic_pointer_cast<Model>(base);
            if (!s_SphereModel) {
                LH_CORE_WARN("Thumbnail: failed to load Sphere primitive for material bake");
                return false;
            }
            return true;
        }
    }

    // Shared mesh-bake body. Public BakeMesh / BakeMaterial differ only by
    // which model + albedo they pass — the GPU work is identical.
    static Image::LoadResult8 BakeMeshInternal(const std::shared_ptr<Model>& model, Vec4 albedo);

    Image::LoadResult8 BakeMesh(const std::shared_ptr<Model>& model)
    {
        if (!s_Initialized) return {};
        // Mesh bakes have no per-asset texture — bind the white default so the
        // shader's texture × albedo multiply degrades to the flat tint.
        auto vkWhite = std::static_pointer_cast<VKTexture>(s_WhiteTexture);
        UpdateSamplerDescriptor(vkWhite->GetImageView(), vkWhite->GetSampler());
        return BakeMeshInternal(model, Vec4(0.85f, 0.85f, 0.85f, 1.0f));
    }

    Image::LoadResult8 BakeMaterial(const std::shared_ptr<Material>& material)
    {
        if (!s_Initialized || !material) return {};
        if (!LoadSphereLazy()) return {};

        // invariant: LoadImmediate every sampled texture, then vkDeviceWaitIdle
        // so async upload fences retire before sampling (else UB). Full GPU
        // sync per material bake; paired with the cache's 1 GPU-bake/frame
        // budget, one sync per affected frame.
        for (const auto& m : material->GetTextures()) {
            if (m.Uuid.IsValid()) AssetManager::LoadImmediate(m.Uuid);
        }
        vkDeviceWaitIdle(VulkanContext::Get().GetDevice());

        // Resolve albedo — fall back to white when material has no diffuse map.
        std::shared_ptr<Texture> albedo = material->GetTextureByType(MapType::Diffuse);
        if (!albedo) albedo = s_WhiteTexture;
        auto vkAlbedo = std::static_pointer_cast<VKTexture>(albedo);
        UpdateSamplerDescriptor(vkAlbedo->GetImageView(), vkAlbedo->GetSampler());

        return BakeMeshInternal(s_SphereModel, material->GetColor());
    }

    static Image::LoadResult8 BakeMeshInternal(const std::shared_ptr<Model>& model, Vec4 albedo)
    {
        Image::LoadResult8 out;
        if (!s_Initialized || !model) return out;

        // Collect every sub-mesh (FBX / GLTF assets routinely split a single
        // model across body / hair / glass / wheels / etc.). Single bake pass
        // binds pipeline + push constants once and switches VBO/IBO per draw.
        struct DrawEntry { VkBuffer vb; VkBuffer ib; u32 indexCount; };
        std::vector<DrawEntry> draws;
        const auto& meshes = model->GetMeshes();
        draws.reserve(meshes.size());
        for (const auto& meshPtr : meshes) {
            if (!meshPtr) continue;
            auto vkVB = std::dynamic_pointer_cast<VKVertexBuffer>(meshPtr->GetVertexBuffer());
            auto vkIB = std::dynamic_pointer_cast<VKIndexBuffer>(meshPtr->GetIndexBuffer());
            if (!vkVB || !vkIB) continue;
            VkBuffer vbBuf = vkVB->GetVulkanBuffer();
            VkBuffer ibBuf = vkIB->GetVulkanBuffer();
            u32      ic    = vkIB->GetCount();
            if (vbBuf == VK_NULL_HANDLE || ibBuf == VK_NULL_HANDLE || ic == 0) continue;
            draws.push_back({ vbBuf, ibBuf, ic });
        }
        if (draws.empty()) return out;

        // Combined bind-pose AABB drives camera orbit fit — model-wide, not
        // first-mesh-only, so multi-mesh models frame correctly.
        AABB aabb;
        for (const auto& md : model->GetMeshesData()) {
            if (!md.BindPoseAABB.IsValid()) continue;
            if (!aabb.IsValid()) {
                aabb = md.BindPoseAABB;
            } else {
                aabb.Expand(md.BindPoseAABB.Min);
                aabb.Expand(md.BindPoseAABB.Max);
            }
        }
        if (!aabb.IsValid()) aabb = AABB{ Vec3(-0.5f), Vec3(0.5f) };

        // Camera fit: use the AABB's largest half-axis, not the bounding-sphere
        // radius (Extents().Length() = sqrt(3)*half-axis for a uniform shape,
        // which would push the camera ~1.73x too far). Tight 10% padding so
        // the model nearly fills the frame.
        const Vec3 center = aabb.Center();
        const Vec3 ext    = aabb.Extents();
        const f32  maxAxis = std::max({ 0.01f, ext.x, ext.y, ext.z });
        const f32  fov    = Math::Radians(45.0f);
        const f32  dist   = (maxAxis / std::tan(fov * 0.5f)) * 1.1f;
        const Vec3 eye    = center + Math::Normalize(Vec3(1.0f, 0.7f, 1.0f)) * dist;
        Mat4       view   = Math::LookAt(eye, center, Vec3(0.0f, 1.0f, 0.0f));
        // Far plane stays generous to handle the bounding-sphere worst case
        // (elongated models seen along the long axis).
        const f32  diag   = Math::Length(ext);
        Mat4       proj   = Math::Perspective(fov, 1.0f, dist * 0.05f, dist * 5.0f + diag * 4.0f);
        proj[1][1] *= -1.0f;   // Vulkan flips Y vs GL convention

        PushConstants pc;
        pc.viewProj = proj * view;
        pc.albedo   = albedo;

        auto vkColor = std::static_pointer_cast<VKTexture>(s_ColorRT);
        auto vkDepth = std::static_pointer_cast<VKTexture>(s_DepthRT);

        VulkanContext::Get().ImmediateSubmit([&](VkCommandBuffer cmd) {
            // Color: SHADER_READ_ONLY_OPTIMAL → COLOR_ATTACHMENT_OPTIMAL
            Barrier(cmd, vkColor->GetImage(), VK_IMAGE_ASPECT_COLOR_BIT,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);

            // Depth: DEPTH_STENCIL_READ_ONLY_OPTIMAL → DEPTH_ATTACHMENT_OPTIMAL
            Barrier(cmd, vkDepth->GetImage(), VK_IMAGE_ASPECT_DEPTH_BIT,
                    VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
                    VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                    0, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                    VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT);

            VkRenderingAttachmentInfo color{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
            color.imageView   = vkColor->GetImageView();
            color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            color.loadOp      = VK_ATTACHMENT_LOAD_OP_CLEAR;
            color.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;
            color.clearValue.color = {{ 0.13f, 0.13f, 0.15f, 1.0f }};

            VkRenderingAttachmentInfo depth{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
            depth.imageView   = vkDepth->GetImageView();
            depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
            depth.loadOp      = VK_ATTACHMENT_LOAD_OP_CLEAR;
            depth.storeOp     = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            depth.clearValue.depthStencil = { 1.0f, 0 };

            VkRenderingInfo info{ VK_STRUCTURE_TYPE_RENDERING_INFO };
            info.renderArea.offset    = { 0, 0 };
            info.renderArea.extent    = { kSize, kSize };
            info.layerCount           = 1;
            info.colorAttachmentCount = 1;
            info.pColorAttachments    = &color;
            info.pDepthAttachment     = &depth;

            vkCmdBeginRendering(cmd, &info);

            VkViewport vp{};
            vp.width    = static_cast<f32>(kSize);
            vp.height   = static_cast<f32>(kSize);
            vp.maxDepth = 1.0f;
            vkCmdSetViewport(cmd, 0, 1, &vp);
            VkRect2D sc{};
            sc.extent = { kSize, kSize };
            vkCmdSetScissor(cmd, 0, 1, &sc);

            VKPipeline* pipeline = model->IsSkinned() ? s_PipelineSkinned.get()
                                                       : s_PipelineStatic.get();
            pipeline->Bind(cmd);

            // Bind the per-bake sampler descriptor — caller updated it before
            // ImmediateSubmit to point at the right texture (white default for
            // mesh, material's albedo for material).
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                pipeline->GetLayout(), 0, 1, &s_SamplerSet, 0, nullptr);

            vkCmdPushConstants(cmd, pipeline->GetLayout(),
                VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                0, sizeof(pc), &pc);

            VkDeviceSize offsets[] = { 0 };
            for (const auto& d : draws) {
                vkCmdBindVertexBuffers(cmd, 0, 1, &d.vb, offsets);
                vkCmdBindIndexBuffer(cmd, d.ib, 0, VK_INDEX_TYPE_UINT32);
                vkCmdDrawIndexed(cmd, d.indexCount, 1, 0, 0, 0);
            }

            vkCmdEndRendering(cmd);

            // Color: COLOR_ATTACHMENT_OPTIMAL → TRANSFER_SRC_OPTIMAL
            Barrier(cmd, vkColor->GetImage(), VK_IMAGE_ASPECT_COLOR_BIT,
                    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT);

            VkBufferImageCopy region{};
            region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            region.imageSubresource.layerCount = 1;
            region.imageExtent                 = { kSize, kSize, 1 };
            vkCmdCopyImageToBuffer(cmd, vkColor->GetImage(),
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s_Staging, 1, &region);

            // Restore both targets to their canonical "ready for sampling" layouts
            // so the next bake sees consistent starting state.
            Barrier(cmd, vkColor->GetImage(), VK_IMAGE_ASPECT_COLOR_BIT,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
            Barrier(cmd, vkDepth->GetImage(), VK_IMAGE_ASPECT_DEPTH_BIT,
                    VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                    VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
                    VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, 0,
                    VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        });

        // Read back from the persistently-mapped staging buffer.
        out.pixels.assign(static_cast<u8*>(s_StagingMapped),
                          static_cast<u8*>(s_StagingMapped) + kStaging);
        out.width  = kSize;
        out.height = kSize;
        out.valid  = true;
        return out;
    }

    // ── Inspector preview path (no readback, separate RT) ───────────────────

    namespace
    {
        // Lazy-create the inspector ring on first call. Each slot owns its own
        // RT pair, sampler set, ImGui descriptor and primary command buffer.
        // The ImGui descriptors outlive any single render (caller draws
        // ImGui::Image referencing them), so they're registered once and reused.
        bool EnsureInspectorRing()
        {
            if (s_InspectorCmdPool != VK_NULL_HANDLE) return true;
            if (!s_Initialized || !VulkanActive()) return false;

            VkDevice device = VulkanContext::Get().GetDevice();

            // Inspector sampler layout/pool — separate from the bake path's
            // single set so per-slot writes don't race with bake submissions.
            {
                VkDescriptorSetLayoutBinding binding{};
                binding.binding         = 0;
                binding.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                binding.descriptorCount = 1;
                binding.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

                VkDescriptorSetLayoutCreateInfo layoutInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
                layoutInfo.bindingCount = 1;
                layoutInfo.pBindings    = &binding;
                if (vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &s_InspectorSamplerLayout) != VK_SUCCESS)
                    return false;

                VkDescriptorPoolSize poolSize{};
                poolSize.type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                poolSize.descriptorCount = kInspectorRingSize;

                VkDescriptorPoolCreateInfo poolInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
                poolInfo.maxSets       = kInspectorRingSize;
                poolInfo.poolSizeCount = 1;
                poolInfo.pPoolSizes    = &poolSize;
                if (vkCreateDescriptorPool(device, &poolInfo, nullptr, &s_InspectorSamplerPool) != VK_SUCCESS)
                    return false;
            }

            // Cmd pool with RESET_COMMAND_BUFFER_BIT — slot buffers are reused
            // per submission rather than freed/reallocated.
            {
                VkCommandPoolCreateInfo poolInfo{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
                poolInfo.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
                poolInfo.queueFamilyIndex = VulkanContext::Get().GetGraphicsFamily();
                if (vkCreateCommandPool(device, &poolInfo, nullptr, &s_InspectorCmdPool) != VK_SUCCESS)
                    return false;
            }

            // Allocate per-slot resources: cmd buffer, sampler set, RT pair,
            // ImGui descriptor for the color view.
            VkCommandBuffer cmdBufs[kInspectorRingSize]{};
            {
                VkCommandBufferAllocateInfo allocInfo{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
                allocInfo.commandPool        = s_InspectorCmdPool;
                allocInfo.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
                allocInfo.commandBufferCount = kInspectorRingSize;
                if (vkAllocateCommandBuffers(device, &allocInfo, cmdBufs) != VK_SUCCESS)
                    return false;
            }

            VkDescriptorSet samplerSets[kInspectorRingSize]{};
            {
                VkDescriptorSetLayout layouts[kInspectorRingSize];
                for (u32 i = 0; i < kInspectorRingSize; ++i) layouts[i] = s_InspectorSamplerLayout;

                VkDescriptorSetAllocateInfo alloc{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
                alloc.descriptorPool     = s_InspectorSamplerPool;
                alloc.descriptorSetCount = kInspectorRingSize;
                alloc.pSetLayouts        = layouts;
                if (vkAllocateDescriptorSets(device, &alloc, samplerSets) != VK_SUCCESS)
                    return false;
            }

            for (u32 i = 0; i < kInspectorRingSize; ++i) {
                auto& slot = s_Ring[i];
                slot.cmd        = cmdBufs[i];
                slot.samplerSet = samplerSets[i];
                slot.color      = Texture::Create(kInspectorSize, kInspectorSize, TextureFormat::RGBA8);
                slot.depth      = Texture::Create(kInspectorSize, kInspectorSize, TextureFormat::D32_Float);
                if (!slot.color || !slot.depth) return false;
            }

            // Texture::Create dispatches uploads async; flush once so first
            // samples and first layout transitions are well-defined. Paid
            // once at first inspector open.
            vkDeviceWaitIdle(device);

            for (u32 i = 0; i < kInspectorRingSize; ++i) {
                auto& slot = s_Ring[i];
                auto vkColor = std::static_pointer_cast<VKTexture>(slot.color);
                slot.imguiSet = ImGui_ImplVulkan_AddTexture(
                    vkColor->GetSampler(),
                    vkColor->GetImageView(),
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                if (slot.imguiSet == VK_NULL_HANDLE) return false;
            }

            s_InspectorTimeline.Init(0);
            s_NextSubmitValue = 0;
            s_RingHead        = 0;
            return true;
        }

        // Eye/view from AABB + orbit. Distance derived from largest half-axis
        // (matches BakeMeshInternal's fit math); azimuth/elevation orbit around
        // the AABB center; distMul scales the tight-fit baseline.
        Mat4 BuildOrbitView(const AABB& aabb, const ThumbnailPreviewScene::OrbitCamera& orb,
                            f32 fovRad, f32& outDist)
        {
            const Vec3 center  = aabb.Center();
            const Vec3 ext     = aabb.Extents();
            const f32  maxAxis = std::max({ 0.01f, ext.x, ext.y, ext.z });
            outDist            = (maxAxis / std::tan(fovRad * 0.5f)) * orb.distMul;

            // Spherical coords: azimuth around Y, elevation above XZ plane.
            const f32 cosE = std::cos(orb.elevation);
            const Vec3 dir = {
                cosE * std::sin(orb.azimuth),
                std::sin(orb.elevation),
                cosE * std::cos(orb.azimuth),
            };
            const Vec3 eye = center + dir * outDist;
            return Math::LookAt(eye, center, Vec3(0.0f, 1.0f, 0.0f));
        }

        ImTextureID RenderInspectorInternal(const std::shared_ptr<Model>& model,
                                            Vec4 albedo,
                                            const ThumbnailPreviewScene::OrbitCamera& orb,
                                            VkImageView samplerView,
                                            VkSampler   samplerHandle)
        {
            if (!s_Initialized || !model) return s_LastGoodInspectorTex;
            if (!EnsureInspectorRing())   return s_LastGoodInspectorTex;

            struct DrawEntry { VkBuffer vb; VkBuffer ib; u32 indexCount; };
            std::vector<DrawEntry> draws;
            const auto& meshes = model->GetMeshes();
            draws.reserve(meshes.size());
            for (const auto& meshPtr : meshes) {
                if (!meshPtr) continue;
                auto vkVB = std::dynamic_pointer_cast<VKVertexBuffer>(meshPtr->GetVertexBuffer());
                auto vkIB = std::dynamic_pointer_cast<VKIndexBuffer>(meshPtr->GetIndexBuffer());
                if (!vkVB || !vkIB) continue;
                VkBuffer vbBuf = vkVB->GetVulkanBuffer();
                VkBuffer ibBuf = vkIB->GetVulkanBuffer();
                u32      ic    = vkIB->GetCount();
                if (vbBuf == VK_NULL_HANDLE || ibBuf == VK_NULL_HANDLE || ic == 0) continue;
                draws.push_back({ vbBuf, ibBuf, ic });
            }
            if (draws.empty()) return s_LastGoodInspectorTex;

            AABB aabb;
            for (const auto& md : model->GetMeshesData()) {
                if (!md.BindPoseAABB.IsValid()) continue;
                if (!aabb.IsValid()) aabb = md.BindPoseAABB;
                else { aabb.Expand(md.BindPoseAABB.Min); aabb.Expand(md.BindPoseAABB.Max); }
            }
            if (!aabb.IsValid()) aabb = AABB{ Vec3(-0.5f), Vec3(0.5f) };

            const f32 fov = Math::Radians(45.0f);
            f32 dist = 0.0f;
            const Mat4 view = BuildOrbitView(aabb, orb, fov, dist);
            const f32 diag = Math::Length(aabb.Extents());
            Mat4 proj = Math::Perspective(fov, 1.0f, dist * 0.05f, dist * 5.0f + diag * 4.0f);
            proj[1][1] *= -1.0f;

            PushConstants pc;
            pc.viewProj = proj * view;
            pc.albedo   = albedo;

            // Pick slot; wait on its previous submission so its cmd buffer and
            // sampler-set descriptor are safe to overwrite.
            const u32 slotIdx = s_RingHead % kInspectorRingSize;
            auto&     slot    = s_Ring[slotIdx];
            if (slot.timelineValue != 0 && s_InspectorTimeline.GetValue() < slot.timelineValue)
                s_InspectorTimeline.Wait(slot.timelineValue);

            UpdateInspectorSamplerSet(slot.samplerSet, samplerView, samplerHandle);

            auto vkColor = std::static_pointer_cast<VKTexture>(slot.color);
            auto vkDepth = std::static_pointer_cast<VKTexture>(slot.depth);

            vkResetCommandBuffer(slot.cmd, 0);
            VkCommandBufferBeginInfo beginInfo{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
            beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            vkBeginCommandBuffer(slot.cmd, &beginInfo);

            VkCommandBuffer cmd = slot.cmd;
            Barrier(cmd, vkColor->GetImage(), VK_IMAGE_ASPECT_COLOR_BIT,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
            Barrier(cmd, vkDepth->GetImage(), VK_IMAGE_ASPECT_DEPTH_BIT,
                    VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
                    VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                    0, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                    VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT);

            VkRenderingAttachmentInfo colorAtt{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
            colorAtt.imageView   = vkColor->GetImageView();
            colorAtt.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            colorAtt.loadOp      = VK_ATTACHMENT_LOAD_OP_CLEAR;
            colorAtt.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;
            colorAtt.clearValue.color = {{ 0.13f, 0.13f, 0.15f, 1.0f }};

            VkRenderingAttachmentInfo depthAtt{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
            depthAtt.imageView   = vkDepth->GetImageView();
            depthAtt.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
            depthAtt.loadOp      = VK_ATTACHMENT_LOAD_OP_CLEAR;
            depthAtt.storeOp     = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            depthAtt.clearValue.depthStencil = { 1.0f, 0 };

            VkRenderingInfo info{ VK_STRUCTURE_TYPE_RENDERING_INFO };
            info.renderArea.offset    = { 0, 0 };
            info.renderArea.extent    = { kInspectorSize, kInspectorSize };
            info.layerCount           = 1;
            info.colorAttachmentCount = 1;
            info.pColorAttachments    = &colorAtt;
            info.pDepthAttachment     = &depthAtt;

            vkCmdBeginRendering(cmd, &info);

            VkViewport vp{};
            vp.width    = static_cast<f32>(kInspectorSize);
            vp.height   = static_cast<f32>(kInspectorSize);
            vp.maxDepth = 1.0f;
            vkCmdSetViewport(cmd, 0, 1, &vp);
            VkRect2D sc{};
            sc.extent = { kInspectorSize, kInspectorSize };
            vkCmdSetScissor(cmd, 0, 1, &sc);

            VKPipeline* pipeline = model->IsSkinned() ? s_PipelineSkinned.get()
                                                      : s_PipelineStatic.get();
            pipeline->Bind(cmd);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                pipeline->GetLayout(), 0, 1, &slot.samplerSet, 0, nullptr);
            vkCmdPushConstants(cmd, pipeline->GetLayout(),
                VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                0, sizeof(pc), &pc);

            VkDeviceSize offsets[] = { 0 };
            for (const auto& d : draws) {
                vkCmdBindVertexBuffers(cmd, 0, 1, &d.vb, offsets);
                vkCmdBindIndexBuffer(cmd, d.ib, 0, VK_INDEX_TYPE_UINT32);
                vkCmdDrawIndexed(cmd, d.indexCount, 1, 0, 0, 0);
            }

            vkCmdEndRendering(cmd);

            Barrier(cmd, vkColor->GetImage(), VK_IMAGE_ASPECT_COLOR_BIT,
                    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
            Barrier(cmd, vkDepth->GetImage(), VK_IMAGE_ASPECT_DEPTH_BIT,
                    VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                    VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
                    VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, 0,
                    VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

            vkEndCommandBuffer(cmd);

            // Submit via timeline semaphore — no wait semaphores (preview is
            // independent of the swapchain), no fence (timeline signal serves
            // as our completion gate).
            const u64 signalValue = ++s_NextSubmitValue;

            VkCommandBufferSubmitInfo cbi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO };
            cbi.commandBuffer = cmd;

            VkSemaphoreSubmitInfo sigSem{ VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO };
            sigSem.semaphore = s_InspectorTimeline.GetHandle();
            sigSem.value     = signalValue;
            sigSem.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT
                             | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;

            VkSubmitInfo2 si{ VK_STRUCTURE_TYPE_SUBMIT_INFO_2 };
            si.commandBufferInfoCount   = 1;
            si.pCommandBufferInfos      = &cbi;
            si.signalSemaphoreInfoCount = 1;
            si.pSignalSemaphoreInfos    = &sigSem;

            VulkanContext::Get().Submit2(si, VK_NULL_HANDLE);

            slot.timelineValue     = signalValue;
            s_RingHead++;
            s_LastGoodInspectorTex = (ImTextureID)slot.imguiSet;
            return s_LastGoodInspectorTex;
        }
    }

    ImTextureID RenderMeshInspector(const std::shared_ptr<Model>& model, const OrbitCamera& cam)
    {
        if (!s_Initialized || !model) return (ImTextureID)0;
        auto vkWhite = std::static_pointer_cast<VKTexture>(s_WhiteTexture);
        return RenderInspectorInternal(model, Vec4(0.85f, 0.85f, 0.85f, 1.0f), cam,
                                       vkWhite->GetImageView(), vkWhite->GetSampler());
    }

    ImTextureID RenderMaterialInspector(const std::shared_ptr<Material>& material, const OrbitCamera& cam)
    {
        if (!s_Initialized || !material) return (ImTextureID)0;
        if (!LoadSphereLazy())            return (ImTextureID)0;

        // Material-change gate: only flush async uploads when the inspected
        // material's identity or its texture-map UUID set changes. Steady
        // state (same material, no edits) does no work here.
        const UUID matHandle = material->Handle;
        size_t texHash = 0;
        {
            UUIDHash hasher;
            for (const auto& m : material->GetTextures()) {
                const size_t h = hasher(m.Uuid);
                texHash ^= h + 0x9e3779b97f4a7c15ULL + (texHash << 6) + (texHash >> 2);
            }
        }
        if (matHandle != s_LastInspectorMaterialHandle ||
            texHash   != s_LastInspectorMaterialTexHash)
        {
            for (const auto& m : material->GetTextures()) {
                if (m.Uuid.IsValid()) AssetManager::LoadImmediate(m.Uuid);
            }
            vkDeviceWaitIdle(VulkanContext::Get().GetDevice());
            s_LastInspectorMaterialHandle  = matHandle;
            s_LastInspectorMaterialTexHash = texHash;
        }

        std::shared_ptr<Texture> albedo = material->GetTextureByType(MapType::Diffuse);
        if (!albedo) albedo = s_WhiteTexture;
        auto vkAlbedo = std::static_pointer_cast<VKTexture>(albedo);

        return RenderInspectorInternal(s_SphereModel, material->GetColor(), cam,
                                       vkAlbedo->GetImageView(), vkAlbedo->GetSampler());
    }

    u32 GetInspectorSize() { return kInspectorSize; }
}
