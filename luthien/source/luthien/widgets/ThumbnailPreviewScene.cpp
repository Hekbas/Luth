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
#include "luth/renderer/resources/Mesh.h"
#include "luth/renderer/resources/Model.h"
#include "luth/renderer/resources/Texture.h"
#include "luth/renderer/shader/ShaderLibrary.h"
#include "luth/renderer/shader/Shader.h"

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
            Mat4 model;
        };
        static_assert(sizeof(PushConstants) == 128, "thumbnail PC must fit minMaxPushConstantsSize");

        // Two pipelines share the same shaders (Position@0 + Normal@12 are at
        // identical offsets in Vertex and SkinnedVertex); only the binding's
        // stride differs. Skinned bakes render bind pose — bone data ignored.
        bool                        s_Initialized = false;
        std::unique_ptr<VKPipeline> s_PipelineStatic;
        std::unique_ptr<VKPipeline> s_PipelineSkinned;
        std::shared_ptr<Texture>    s_ColorRT;
        std::shared_ptr<Texture>    s_DepthRT;
        VkBuffer                    s_Staging       = VK_NULL_HANDLE;
        VmaAllocation               s_StagingAlloc  = nullptr;
        void*                       s_StagingMapped = nullptr;

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

            // Position@0 and Normal@12 are at identical offsets in Vertex and
            // SkinnedVertex; only the per-vertex stride differs.
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

            cfg.attributeDescriptions = { pos, nrm };

            VkPushConstantRange pc{};
            pc.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
            pc.offset     = 0;
            pc.size       = sizeof(PushConstants);
            cfg.pushConstantRanges = { pc };

            return std::make_unique<VKPipeline>(cfg, vert, frag,
                                                std::vector<VkDescriptorSetLayout>{});
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

        if (!CreatePipelines()) { Shutdown(); return false; }
        if (!CreateTargets())  { Shutdown(); return false; }
        if (!CreateStaging())  { Shutdown(); return false; }

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
        if (s_StagingAlloc) {
            if (s_StagingMapped) VulkanAllocator::Unmap(s_StagingAlloc);
            VulkanAllocator::FreeBuffer(s_Staging, s_StagingAlloc);
            s_Staging       = VK_NULL_HANDLE;
            s_StagingAlloc  = nullptr;
            s_StagingMapped = nullptr;
        }
    }

    u32 GetSize() { return kSize; }

    Image::LoadResult8 BakeMesh(const std::shared_ptr<Model>& model)
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

        const Vec3 center = aabb.Center();
        const f32  radius = std::max(0.01f, Math::Length(aabb.Extents()));
        const f32  fov    = Math::Radians(45.0f);
        const f32  dist   = (radius / std::tan(fov * 0.5f)) * 1.4f;
        const Vec3 eye    = center + Math::Normalize(Vec3(1.0f, 0.7f, 1.0f)) * dist;
        Mat4       view   = Math::LookAt(eye, center, Vec3(0.0f, 1.0f, 0.0f));
        Mat4       proj   = Math::Perspective(fov, 1.0f, dist * 0.05f, dist * 5.0f + radius * 4.0f);
        proj[1][1] *= -1.0f;   // Vulkan flips Y vs GL convention

        PushConstants pc;
        pc.viewProj = proj * view;
        pc.model    = Mat4(1.0f);

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
}
