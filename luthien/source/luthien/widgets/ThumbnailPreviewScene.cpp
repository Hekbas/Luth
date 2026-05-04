#include "lepch.h"
#include "luthien/widgets/ThumbnailPreviewScene.h"

#include "luth/core/diagnostics/Log.h"
#include "luth/core/types/LuthMath.h"
#include "luth/renderer/Renderer.h"
#include "luth/renderer/RenderBackend.h"
#include "luth/renderer/backend/vulkan/VulkanAllocator.h"
#include "luth/renderer/backend/vulkan/VulkanBuffer.h"
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

        // Live inspector render target — separate from the bake RTs so disk
        // thumbnails (128²) and inspector previews (256²) don't conflict.
        // Lazy-init on first inspector call. Shared single set since only one
        // Material/Model inspector is visible inside InspectorPanel at a time.
        constexpr u32 kInspectorSize = 256;
        std::shared_ptr<Texture>    s_InspectorColorRT;
        std::shared_ptr<Texture>    s_InspectorDepthRT;
        VkDescriptorSet             s_InspectorImGuiSet = VK_NULL_HANDLE;

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
        s_PipelineStatic.reset();   // ~VKPipeline destroys VkPipeline + layout
        s_PipelineSkinned.reset();
        s_ColorRT.reset();          // ~VKTexture pushes image deletion to fenced queue
        s_DepthRT.reset();
        s_InspectorColorRT.reset();
        s_InspectorDepthRT.reset();
        // ImGui descriptor pool is destroyed during Editor::Shutdown alongside
        // ours; skip ImGui_ImplVulkan_RemoveTexture (would race with pool destroy).
        s_InspectorImGuiSet = VK_NULL_HANDLE;
        s_WhiteTexture.reset();
        s_SphereModel.reset();

        VkDevice device = VulkanActive() ? VulkanContext::Get().GetDevice() : VK_NULL_HANDLE;
        if (device != VK_NULL_HANDLE) {
            // Pool destroy frees the allocated set.
            if (s_SamplerPool   != VK_NULL_HANDLE) vkDestroyDescriptorPool(device, s_SamplerPool, nullptr);
            if (s_SamplerLayout != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(device, s_SamplerLayout, nullptr);
        }
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
        // Lazy-create the inspector RTs + ImGui descriptor on first call.
        // The descriptor outlives any single bake (caller draws ImGui::Image
        // referencing it), so we register it once and reuse.
        bool EnsureInspectorRTs()
        {
            if (s_InspectorImGuiSet != VK_NULL_HANDLE) return true;
            if (!s_Initialized || !VulkanActive()) return false;

            s_InspectorColorRT = Texture::Create(kInspectorSize, kInspectorSize, TextureFormat::RGBA8);
            s_InspectorDepthRT = Texture::Create(kInspectorSize, kInspectorSize, TextureFormat::D32_Float);
            if (!s_InspectorColorRT || !s_InspectorDepthRT) {
                s_InspectorColorRT.reset();
                s_InspectorDepthRT.reset();
                return false;
            }

            // Texture::Create dispatches the upload async; flush so the first
            // sample isn't UB. Cost is paid once on first inspector open.
            vkDeviceWaitIdle(VulkanContext::Get().GetDevice());

            auto vkColor = std::static_pointer_cast<VKTexture>(s_InspectorColorRT);
            s_InspectorImGuiSet = ImGui_ImplVulkan_AddTexture(
                vkColor->GetSampler(),
                vkColor->GetImageView(),
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            return s_InspectorImGuiSet != VK_NULL_HANDLE;
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
                                            const ThumbnailPreviewScene::OrbitCamera& orb)
        {
            if (!s_Initialized || !model) return (ImTextureID)0;
            if (!EnsureInspectorRTs())    return (ImTextureID)0;

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
            if (draws.empty()) return (ImTextureID)0;

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

            auto vkColor = std::static_pointer_cast<VKTexture>(s_InspectorColorRT);
            auto vkDepth = std::static_pointer_cast<VKTexture>(s_InspectorDepthRT);

            VulkanContext::Get().ImmediateSubmit([&](VkCommandBuffer cmd) {
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
                info.renderArea.extent    = { kInspectorSize, kInspectorSize };
                info.layerCount           = 1;
                info.colorAttachmentCount = 1;
                info.pColorAttachments    = &color;
                info.pDepthAttachment     = &depth;

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
            });

            return (ImTextureID)s_InspectorImGuiSet;
        }
    }

    ImTextureID RenderMeshInspector(const std::shared_ptr<Model>& model, const OrbitCamera& cam)
    {
        if (!s_Initialized || !model) return (ImTextureID)0;
        auto vkWhite = std::static_pointer_cast<VKTexture>(s_WhiteTexture);
        UpdateSamplerDescriptor(vkWhite->GetImageView(), vkWhite->GetSampler());
        return RenderInspectorInternal(model, Vec4(0.85f, 0.85f, 0.85f, 1.0f), cam);
    }

    ImTextureID RenderMaterialInspector(const std::shared_ptr<Material>& material, const OrbitCamera& cam)
    {
        if (!s_Initialized || !material) return (ImTextureID)0;
        if (!LoadSphereLazy())            return (ImTextureID)0;

        // Sampled textures must be resident before the bake samples them.
        // LoadImmediate is blocking; vkDeviceWaitIdle ensures async upload
        // fences retire so the descriptor write reads valid pixels.
        for (const auto& m : material->GetTextures()) {
            if (m.Uuid.IsValid()) AssetManager::LoadImmediate(m.Uuid);
        }
        vkDeviceWaitIdle(VulkanContext::Get().GetDevice());

        std::shared_ptr<Texture> albedo = material->GetTextureByType(MapType::Diffuse);
        if (!albedo) albedo = s_WhiteTexture;
        auto vkAlbedo = std::static_pointer_cast<VKTexture>(albedo);
        UpdateSamplerDescriptor(vkAlbedo->GetImageView(), vkAlbedo->GetSampler());

        return RenderInspectorInternal(s_SphereModel, material->GetColor(), cam);
    }

    u32 GetInspectorSize() { return kInspectorSize; }
}
