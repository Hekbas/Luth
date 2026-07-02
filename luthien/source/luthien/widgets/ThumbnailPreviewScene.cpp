#include "lepch.h"
#include "luthien/widgets/ThumbnailPreviewScene.h"

#include "luth/core/diagnostics/Log.h"
#include "luth/core/types/LuthMath.h"
#include "luth/renderer/Renderer.h"
#include "luth/renderer/RenderBackend.h"
#include "luth/renderer/backend/vulkan/VulkanAllocator.h"
#include "luth/renderer/backend/vulkan/VulkanBuffer.h"
#include "luth/renderer/backend/vulkan/TimelineSemaphore.h"
#include "luth/renderer/backend/vulkan/UploadContext.h"
#include "luth/renderer/backend/vulkan/VulkanContext.h"
#include "luth/renderer/backend/vulkan/VulkanDescriptors.h"
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
#include <unordered_map>

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
            u32  diffuseIndex;
            u32  _pad[3]; // pad to 16-byte align (push-constant ranges are size-rounded)
            Vec4 emissive; // rgb = factor (linear), a = HDR strength
        };
        static_assert(sizeof(PushConstants) == 112, "thumbnail PC layout drift");

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

        // 1x1 white default texture: bound for mesh bakes (no albedo texture)
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
            VkCommandBuffer          cmd          = VK_NULL_HANDLE;
            u64                      timelineValue = 0;
        };

        std::array<InspectorSlot, kInspectorRingSize> s_Ring{};
        VkCommandPool         s_InspectorCmdPool        = VK_NULL_HANDLE;
        TimelineSemaphore     s_InspectorTimeline;
        u64                   s_NextSubmitValue         = 0;
        u32                   s_RingHead                = 0;
        ImTextureID           s_LastGoodInspectorTex    = (ImTextureID)0;

        // Material-change gate keys. `RenderMaterialInspector` re-runs
        // LoadImmediate + vkDeviceWaitIdle only when the inspected material's
        // identity or its texture-map UUID set changes.
        UUID                  s_LastInspectorMaterialHandle = UUID::Invalid();
        size_t                s_LastInspectorMaterialTexHash = 0;

        // ---- Graph-aware preview ----
        // A graph material previews through its generated Lambert-over-graph fragment (one pipeline per
        // structure, keyed by the material's preview-shader UUID) reading a self-contained per-call UBO;
        // the preview submits off the frame loop, so it can't bind MaterialSystem's transient Set 2.
        std::vector<u32>      s_GraphVertSpv;
        VkDescriptorSetLayout s_PreviewUboLayout = VK_NULL_HANDLE;
        VkDescriptorPool      s_PreviewUboPool   = VK_NULL_HANDLE;
        std::unordered_map<UUID, std::unique_ptr<VKPipeline>, UUIDHash> s_GraphPipelines;
        std::unordered_map<UUID, std::vector<u32>, UUIDHash>           s_PreviewSpvCache;

        // CPU mirror of material_bindings_preview's PreviewMaterialUBO (std140 == std430 for this struct).
        struct PreviewMaterialUBOData
        {
            GPUMaterialData m;
            Vec4 params[MAT_GRAPH_STRIDE];
        };

        // One host-visible UBO + its static descriptor set (set 0). Per inspector ring slot (synced by the
        // slot's timeline wait) + one for the synchronous bake.
        struct PreviewUbo
        {
            VkBuffer        buf    = VK_NULL_HANDLE;
            VmaAllocation   alloc  = nullptr;
            void*           mapped = nullptr;
            VkDescriptorSet set    = VK_NULL_HANDLE;
        };
        PreviewUbo                                 s_BakeUbo;
        std::array<PreviewUbo, kInspectorRingSize> s_InspectorUbos{};

        bool VulkanActive()
        {
            return Renderer::GetBackend()
                && Renderer::GetBackend()->GetAPI() == RenderBackend::API::Vulkan;
        }

        bool LoadShader(const char* relPath, std::vector<u32>& out)
        {
            auto sh = ShaderLibrary::LoadEngine(relPath);
            if (!sh) { LH_LOG(Editor, error, "Thumbnail: failed to load engine shader '{}'", relPath); return false; }
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

            // Pipeline references the engine-wide bindless descriptor set layout (Set 1 in
            // main rendering, Set 0 here; same VkDescriptorSetLayout, different binding index
            // at the pipeline-layout level). Per-bake sampling uses globalTextures[diffuseIndex].
            return std::make_unique<VKPipeline>(cfg, vert, frag,
                std::vector<VkDescriptorSetLayout>{ VulkanContext::Get().GetBindlessSet().GetLayout() });
        }

        bool CreateWhiteTexture()
        {
            // Async-upload path; flush with vkDeviceWaitIdle below so the first sample isn't
            // UB. Drain the deferred-bindless pump so s_WhiteTexture's bindless slot is
            // populated before any bake fetches it via GetBindlessIndex(). Init runs once at
            // editor startup so the wait has no per-frame cost.
            const u8 white[4] = { 255, 255, 255, 255 };
            s_WhiteTexture = Texture::Create(1, 1, TextureFormat::RGBA8, white);
            if (!s_WhiteTexture) return false;
            vkDeviceWaitIdle(VulkanContext::Get().GetDevice());
            UploadContext::Get().DrainPendingBinds();
            return true;
        }

        // Resolves a Texture to a GPU-safe bindless slot. Falls back to slot 0 (1x1 white)
        // when the texture is missing OR not yet registered; visually equivalent to the old
        // s_WhiteTexture binding for both cases.
        u32 ResolveBindlessIndex(const std::shared_ptr<Texture>& tex)
        {
            if (!tex) return 0u;
            auto vkTex = std::static_pointer_cast<VKTexture>(tex);
            return BindlessOrNull(vkTex->GetBindlessIndex());
        }

        bool CreatePipelines()
        {
            std::vector<u32> vert, frag;
            if (!LoadShader("shaders/thumbnail_mesh_vert.slang", vert)) return false;
            if (!LoadShader("shaders/thumbnail_mesh.slang", frag)) return false;

            s_PipelineStatic  = BuildPipeline(vert, frag, kStaticVertexStride);
            s_PipelineSkinned = BuildPipeline(vert, frag, kSkinnedVertexStride);
            // Graph-preview vertex (adds tangent + UV1); per-material frags pair with it lazily.
            LoadShader("shaders/thumbnail_graph.slang", s_GraphVertSpv);
            return s_PipelineStatic && s_PipelineSkinned;
        }

        // Maps a material's preview-shader UUID to its generated SPIR-V (ShaderLibrary scan, cached);
        // mirrors GeometrySubsystem::ResolveFragSpv. Empty until codegen registers the consumer.
        const std::vector<u32>& ResolvePreviewSpv(const UUID& uuid)
        {
            static const std::vector<u32> kEmpty;
            if (!uuid.IsValid()) return kEmpty;
            auto it = s_PreviewSpvCache.find(uuid);
            if (it != s_PreviewSpvCache.end()) return it->second;
            for (const auto& [name, sh] : ShaderLibrary::GetAll())
                if (sh && sh->Handle == uuid && !sh->GetSpirV().empty())
                    return s_PreviewSpvCache.emplace(uuid, sh->GetSpirV()).first->second;
            return kEmpty;
        }

        // Lambert-over-graph pipeline: graph vert (5 attrs) + the per-material frag, two-set layout
        // { preview UBO @0, bindless @1 }, vertex-only mat4 push. Static stride (material previews
        // render the static sphere, no skinned graph variant needed).
        std::unique_ptr<VKPipeline> BuildGraphPipeline(const std::vector<u32>& frag)
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
            bind.stride    = kStaticVertexStride;
            bind.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
            cfg.bindingDescriptions = { bind };

            auto attr = [](u32 loc, VkFormat fmt, u32 off) {
                VkVertexInputAttributeDescription a{};
                a.location = loc; a.binding = 0; a.format = fmt; a.offset = off;
                return a;
            };
            cfg.attributeDescriptions = {
                attr(0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, Position)),
                attr(1, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, Normal)),
                attr(2, VK_FORMAT_R32G32_SFLOAT,    offsetof(Vertex, TexCoord0)),
                attr(3, VK_FORMAT_R32G32_SFLOAT,    offsetof(Vertex, TexCoord1)),
                attr(4, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, Tangent)),
            };

            VkPushConstantRange pc{};
            pc.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
            pc.offset     = 0;
            pc.size       = sizeof(Mat4);
            cfg.pushConstantRanges = { pc };

            return std::make_unique<VKPipeline>(cfg, s_GraphVertSpv, frag,
                std::vector<VkDescriptorSetLayout>{ s_PreviewUboLayout,
                                                    VulkanContext::Get().GetBindlessSet().GetLayout() });
        }

        // Cached graph pipeline for a preview UUID; nullptr until codegen registers + the vert is loaded.
        VKPipeline* GetOrBuildGraphPipeline(const UUID& previewUUID)
        {
            if (!previewUUID.IsValid() || s_GraphVertSpv.empty() || s_PreviewUboLayout == VK_NULL_HANDLE) return nullptr;
            auto it = s_GraphPipelines.find(previewUUID);
            if (it != s_GraphPipelines.end()) return it->second.get();
            const std::vector<u32>& frag = ResolvePreviewSpv(previewUUID);
            if (frag.empty()) return nullptr;   // not yet registered: Lambert this frame, retried next
            auto pipe = BuildGraphPipeline(frag);
            VKPipeline* raw = pipe.get();
            s_GraphPipelines.emplace(previewUUID, std::move(pipe));
            return raw;
        }

        void FillPreviewUbo(PreviewUbo& ubo, const Material* mat)
        {
            PreviewMaterialUBOData data{};
            data.m = mat->GetGPUData();
            const auto& gp = mat->GetGraphParams();
            for (size_t k = 0; k < gp.size() && k < MAT_GRAPH_STRIDE; ++k) data.params[k] = gp[k];
            std::memcpy(ubo.mapped, &data, sizeof(data));
        }

        // UBO layout + pool + one host-visible UBO/descriptor per ring slot + the bake. Static across the
        // session; only the buffer contents change per call (per-slot sync rides the ring's timeline wait).
        bool CreatePreviewResources()
        {
            VkDevice device = VulkanContext::Get().GetDevice();

            VkDescriptorSetLayoutBinding b{};
            b.binding         = 0;
            b.descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            b.descriptorCount = 1;
            b.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
            VkDescriptorSetLayoutCreateInfo lci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
            lci.bindingCount = 1;
            lci.pBindings    = &b;
            if (vkCreateDescriptorSetLayout(device, &lci, nullptr, &s_PreviewUboLayout) != VK_SUCCESS)
                return false;

            const u32 count = 1 + kInspectorRingSize;
            VkDescriptorPoolSize ps{ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, count };
            VkDescriptorPoolCreateInfo pci{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
            pci.maxSets       = count;
            pci.poolSizeCount = 1;
            pci.pPoolSizes    = &ps;
            if (vkCreateDescriptorPool(device, &pci, nullptr, &s_PreviewUboPool) != VK_SUCCESS)
                return false;

            auto makeUbo = [&](PreviewUbo& u) -> bool {
                VkBufferCreateInfo bi{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
                bi.size  = sizeof(PreviewMaterialUBOData);
                bi.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
                u.alloc = VulkanAllocator::AllocateBuffer(bi, VMA_MEMORY_USAGE_CPU_ONLY, u.buf);
                if (!u.alloc) return false;
                u.mapped = VulkanAllocator::Map(u.alloc);
                if (!u.mapped) return false;
                VkDescriptorSetAllocateInfo ai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
                ai.descriptorPool     = s_PreviewUboPool;
                ai.descriptorSetCount = 1;
                ai.pSetLayouts        = &s_PreviewUboLayout;
                if (vkAllocateDescriptorSets(device, &ai, &u.set) != VK_SUCCESS) return false;
                VkDescriptorBufferInfo dbi{ u.buf, 0, sizeof(PreviewMaterialUBOData) };
                VkWriteDescriptorSet w{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
                w.dstSet          = u.set;
                w.dstBinding      = 0;
                w.descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
                w.descriptorCount = 1;
                w.pBufferInfo     = &dbi;
                vkUpdateDescriptorSets(device, 1, &w, 0, nullptr);
                return true;
            };
            if (!makeUbo(s_BakeUbo)) return false;
            for (auto& u : s_InspectorUbos) if (!makeUbo(u)) return false;
            return true;
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

        if (!CreatePipelines())         { Shutdown(); return false; }
        if (!CreateTargets())           { Shutdown(); return false; }
        if (!CreateStaging())           { Shutdown(); return false; }
        if (!CreateWhiteTexture())      { Shutdown(); return false; }
        // Non-fatal: on failure material graph previews fall back to the stock Lambert shader.
        if (!CreatePreviewResources())
            LH_LOG(Editor, warn, "Thumbnail: graph-preview resources failed - material previews stay Lambert");

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
        s_GraphPipelines.clear();   // each ~VKPipeline destroys its pipeline + layout
        s_PreviewSpvCache.clear();
        s_GraphVertSpv.clear();
        s_ColorRT.reset();          // ~VKTexture pushes image deletion to fenced queue
        s_DepthRT.reset();

        // Inspector ring teardown. ImGui descriptor pool is destroyed during
        // Editor::Shutdown alongside ours; skip ImGui_ImplVulkan_RemoveTexture
        // (would race with pool destroy).
        for (auto& slot : s_Ring) {
            slot.color.reset();
            slot.depth.reset();
            slot.imguiSet     = VK_NULL_HANDLE;
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
            auto freeUbo = [&](PreviewUbo& u) {
                if (u.mapped) { VulkanAllocator::Unmap(u.alloc); u.mapped = nullptr; }
                if (u.alloc)  { VulkanAllocator::FreeBuffer(u.buf, u.alloc); u.buf = VK_NULL_HANDLE; u.alloc = nullptr; }
                u.set = VK_NULL_HANDLE;
            };
            freeUbo(s_BakeUbo);
            for (auto& u : s_InspectorUbos) freeUbo(u);
            // Pool destroy frees the preview UBO sets + the inspector cmd buffers.
            if (s_PreviewUboPool   != VK_NULL_HANDLE) vkDestroyDescriptorPool(device, s_PreviewUboPool, nullptr);
            if (s_PreviewUboLayout != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(device, s_PreviewUboLayout, nullptr);
            if (s_InspectorCmdPool != VK_NULL_HANDLE) vkDestroyCommandPool(device, s_InspectorCmdPool, nullptr);
        }
        s_PreviewUboPool   = VK_NULL_HANDLE;
        s_PreviewUboLayout = VK_NULL_HANDLE;
        s_InspectorCmdPool = VK_NULL_HANDLE;
        s_InspectorTimeline.Shutdown();

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
        // Sphere primitive UUID: luth/assets/models/primitives/Sphere.fbx.
        // Stable across builds via the .meta file shipped with the engine.
        const UUID kSphereUUID = UUID::FromString("4c5ad301-7125-475e-954d-80c64c38a552");

        bool LoadSphereLazy()
        {
            if (s_SphereModel) return true;
            auto base = AssetManager::LoadImmediate(kSphereUUID);
            s_SphereModel = std::dynamic_pointer_cast<Model>(base);
            if (!s_SphereModel) {
                LH_LOG(Editor, warn, "Thumbnail: failed to load Sphere primitive for material bake");
                return false;
            }
            return true;
        }
    }

    // Shared mesh-bake body. Public BakeMesh / BakeMaterial differ only by
    // which model + albedo they pass; the GPU work is identical.
    static Image::LoadResult8 BakeMeshInternal(const std::shared_ptr<Model>& model, Vec4 albedo, u32 diffuseIndex, const Material* mat);

    Image::LoadResult8 BakeMesh(const std::shared_ptr<Model>& model)
    {
        if (!s_Initialized) return {};
        // Mesh bakes have no per-asset texture: sample slot 0 (1x1 white) so
        // shader texture x albedo collapses to the flat tint.
        return BakeMeshInternal(model, Vec4(0.85f, 0.85f, 0.85f, 1.0f),
                                ResolveBindlessIndex(s_WhiteTexture), nullptr);
    }

    Image::LoadResult8 BakeMaterial(const std::shared_ptr<Material>& material)
    {
        if (!s_Initialized || !material) return {};
        if (!LoadSphereLazy()) return {};

        // invariant: LoadImmediate every sampled texture, then vkDeviceWaitIdle so async
        // upload fences retire before sampling. Drain the deferred-bindless pump so the
        // material's diffuse slot is populated before ResolveBindlessIndex() reads it.
        for (const auto& m : material->GetTextures()) {
            if (m.Uuid.IsValid()) AssetManager::LoadImmediate(m.Uuid);
        }
        vkDeviceWaitIdle(VulkanContext::Get().GetDevice());
        UploadContext::Get().DrainPendingBinds();

        std::shared_ptr<Texture> albedo = material->GetTextureByType(MapType::Diffuse);
        material->UpdateGPUData();   // refresh m_GPUData (bindless indices + flags) before the preview UBO fill
        return BakeMeshInternal(s_SphereModel, material->GetColor(),
                                ResolveBindlessIndex(albedo), material.get());
    }

    // One sub-mesh draw plus the node-tree world matrix that places it (identity if no node tree).
    struct ThumbDraw { VkBuffer vb; VkBuffer ib; u32 indexCount; Mat4 model; };

    static void ExpandByTransformedAABB(AABB& out, const Mat4& m, const AABB& box)
    {
        const Vec3 mn = box.Min, mx = box.Max;
        for (int i = 0; i < 8; ++i)
            out.Expand(Vec3(m * Vec4((i & 1) ? mx.x : mn.x, (i & 2) ? mx.y : mn.y, (i & 4) ? mx.z : mn.z, 1.0f)));
    }

    // Sub-mesh draw list + framing AABB in model space. V4 node-tree models place each mesh by its
    // node's world transform (verts are un-baked); skinned / legacy models draw flat at identity.
    static void BuildModelDraws(const std::shared_ptr<Model>& model,
        std::vector<ThumbDraw>& draws, AABB& aabb)
    {
        const auto& meshes   = model->GetMeshes();
        const auto& meshData = model->GetMeshesData();

        auto pushDraw = [&](u32 meshIdx, const Mat4& world) {
            if (meshIdx >= meshes.size() || !meshes[meshIdx]) return;
            auto vkVB = std::dynamic_pointer_cast<VKVertexBuffer>(meshes[meshIdx]->GetVertexBuffer());
            auto vkIB = std::dynamic_pointer_cast<VKIndexBuffer>(meshes[meshIdx]->GetIndexBuffer());
            if (!vkVB || !vkIB) return;
            VkBuffer vb = vkVB->GetVulkanBuffer(), ib = vkIB->GetVulkanBuffer();
            u32 ic = vkIB->GetCount();
            if (vb == VK_NULL_HANDLE || ib == VK_NULL_HANDLE || ic == 0) return;
            draws.push_back({ vb, ib, ic, world });
            if (meshIdx < meshData.size() && meshData[meshIdx].BindPoseAABB.IsValid())
                ExpandByTransformedAABB(aabb, world, meshData[meshIdx].BindPoseAABB);
        };

        if (model->HasNodeTree()) {
            const auto worlds = model->ComputeNodeWorldTransforms();
            const auto& nodes = model->GetNodes();
            for (size_t n = 0; n < nodes.size(); ++n)
                for (u32 mi : nodes[n].MeshIndices)
                    pushDraw(mi, worlds[n]);
        } else {
            for (u32 i = 0; i < (u32)meshes.size(); ++i)
                pushDraw(i, Mat4(1.0f));
        }
    }

    // Records the preview draws: a graph material binds its per-material Lambert-over-graph pipeline + the
    // given per-call UBO (set 0) + bindless (set 1); a non-graph / unresolved material uses the stock Lambert
    // pipeline (bindless @0). Per-draw node-world matrix folds into viewProj either way.
    static void RecordPreviewDraws(VkCommandBuffer cmd, const std::shared_ptr<Model>& model, const Material* mat,
                                   const std::vector<ThumbDraw>& draws, PushConstants pc, PreviewUbo& ubo)
    {
        VkDescriptorSet bindlessSet = VulkanContext::Get().GetBindlessSet().GetSet();
        const Mat4 baseViewProj = pc.viewProj;
        VkDeviceSize offsets[] = { 0 };

        VKPipeline* graphPipe = (mat && mat->GetGraphPreviewShaderUUID().IsValid())
                              ? GetOrBuildGraphPipeline(mat->GetGraphPreviewShaderUUID()) : nullptr;

        if (graphPipe) {
            FillPreviewUbo(ubo, mat);
            graphPipe->Bind(cmd);
            VkDescriptorSet sets[] = { ubo.set, bindlessSet };
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                graphPipe->GetLayout(), 0, 2, sets, 0, nullptr);
            for (const auto& d : draws) {
                Mat4 mvp = baseViewProj * d.model;
                vkCmdPushConstants(cmd, graphPipe->GetLayout(), VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(Mat4), &mvp);
                vkCmdBindVertexBuffers(cmd, 0, 1, &d.vb, offsets);
                vkCmdBindIndexBuffer(cmd, d.ib, 0, VK_INDEX_TYPE_UINT32);
                vkCmdDrawIndexed(cmd, d.indexCount, 1, 0, 0, 0);
            }
        } else {
            VKPipeline* pipeline = model->IsSkinned() ? s_PipelineSkinned.get() : s_PipelineStatic.get();
            pipeline->Bind(cmd);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                pipeline->GetLayout(), 0, 1, &bindlessSet, 0, nullptr);
            for (const auto& d : draws) {
                pc.viewProj = baseViewProj * d.model;
                vkCmdPushConstants(cmd, pipeline->GetLayout(),
                    VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
                vkCmdBindVertexBuffers(cmd, 0, 1, &d.vb, offsets);
                vkCmdBindIndexBuffer(cmd, d.ib, 0, VK_INDEX_TYPE_UINT32);
                vkCmdDrawIndexed(cmd, d.indexCount, 1, 0, 0, 0);
            }
        }
    }

    static Image::LoadResult8 BakeMeshInternal(const std::shared_ptr<Model>& model, Vec4 albedo, u32 diffuseIndex, const Material* mat)
    {
        Image::LoadResult8 out;
        if (!s_Initialized || !model) return out;

        // Collect every sub-mesh (FBX / GLTF assets routinely split a single
        // model across body / hair / glass / wheels / etc.). Single bake pass
        // binds pipeline + push constants once and switches VBO/IBO per draw.
        // Node-aware draws + model-space framing AABB (un-baked V4 meshes placed by node transforms).
        std::vector<ThumbDraw> draws;
        AABB aabb;
        BuildModelDraws(model, draws, aabb);
        if (draws.empty()) return out;
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

        PushConstants pc{};
        pc.viewProj     = proj * view;
        pc.albedo       = albedo;
        pc.diffuseIndex = diffuseIndex;

        auto vkColor = std::static_pointer_cast<VKTexture>(s_ColorRT);
        auto vkDepth = std::static_pointer_cast<VKTexture>(s_DepthRT);

        VulkanContext::Get().ImmediateSubmit([&](VkCommandBuffer cmd) {
            // Color: SHADER_READ_ONLY_OPTIMAL -> COLOR_ATTACHMENT_OPTIMAL
            Barrier(cmd, vkColor->GetImage(), VK_IMAGE_ASPECT_COLOR_BIT,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);

            // Depth: DEPTH_STENCIL_READ_ONLY_OPTIMAL -> DEPTH_ATTACHMENT_OPTIMAL
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

            RecordPreviewDraws(cmd, model, mat, draws, pc, s_BakeUbo);

            vkCmdEndRendering(cmd);

            // Color: COLOR_ATTACHMENT_OPTIMAL -> TRANSFER_SRC_OPTIMAL
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

    // ---- Inspector preview path (no readback, separate RT) ----

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

            // Cmd pool with RESET_COMMAND_BUFFER_BIT: slot buffers are reused
            // per submission rather than freed/reallocated.
            {
                VkCommandPoolCreateInfo poolInfo{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
                poolInfo.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
                poolInfo.queueFamilyIndex = VulkanContext::Get().GetGraphicsFamily();
                if (vkCreateCommandPool(device, &poolInfo, nullptr, &s_InspectorCmdPool) != VK_SUCCESS)
                    return false;
            }

            // Allocate per-slot resources: cmd buffer, RT pair, ImGui descriptor.
            VkCommandBuffer cmdBufs[kInspectorRingSize]{};
            {
                VkCommandBufferAllocateInfo allocInfo{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
                allocInfo.commandPool        = s_InspectorCmdPool;
                allocInfo.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
                allocInfo.commandBufferCount = kInspectorRingSize;
                if (vkAllocateCommandBuffers(device, &allocInfo, cmdBufs) != VK_SUCCESS)
                    return false;
            }

            for (u32 i = 0; i < kInspectorRingSize; ++i) {
                auto& slot = s_Ring[i];
                slot.cmd        = cmdBufs[i];
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
                                            u32 diffuseIndex,
                                            Vec4 emissive,
                                            const Material* mat)
        {
            if (!s_Initialized || !model) return s_LastGoodInspectorTex;
            if (!EnsureInspectorRing())   return s_LastGoodInspectorTex;

            std::vector<ThumbDraw> draws;
            AABB aabb;
            BuildModelDraws(model, draws, aabb);
            if (draws.empty()) return s_LastGoodInspectorTex;
            if (!aabb.IsValid()) aabb = AABB{ Vec3(-0.5f), Vec3(0.5f) };

            const f32 fov = Math::Radians(45.0f);
            f32 dist = 0.0f;
            const Mat4 view = BuildOrbitView(aabb, orb, fov, dist);
            const f32 diag = Math::Length(aabb.Extents());
            Mat4 proj = Math::Perspective(fov, 1.0f, dist * 0.05f, dist * 5.0f + diag * 4.0f);
            proj[1][1] *= -1.0f;

            PushConstants pc{};
            pc.viewProj     = proj * view;
            pc.albedo       = albedo;
            pc.diffuseIndex = diffuseIndex;
            pc.emissive     = emissive;

            // Pick slot; wait on its previous submission so its cmd buffer is safe to overwrite.
            // The engine-wide bindless set carries the texture across submissions; no per-slot
            // descriptor write is needed any more.
            const u32 slotIdx = s_RingHead % kInspectorRingSize;
            auto&     slot    = s_Ring[slotIdx];
            if (slot.timelineValue != 0 && s_InspectorTimeline.GetValue() < slot.timelineValue)
                s_InspectorTimeline.Wait(slot.timelineValue);

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

            RecordPreviewDraws(cmd, model, mat, draws, pc, s_InspectorUbos[slotIdx]);

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

            // Submit via timeline semaphore: no wait semaphores (preview is
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
        return RenderInspectorInternal(model, Vec4(0.85f, 0.85f, 0.85f, 1.0f), cam,
                                       ResolveBindlessIndex(s_WhiteTexture), Vec4(0.0f), nullptr);
    }

    ImTextureID RenderMaterialInspector(const std::shared_ptr<Material>& material, const OrbitCamera& cam)
    {
        if (!s_Initialized || !material) return (ImTextureID)0;
        if (!LoadSphereLazy())            return (ImTextureID)0;

        // Material-change gate: only flush async uploads + drain the bindless pump when the
        // inspected material's identity or its texture-map UUID set changes. Steady state
        // (same material, no edits) does no work here.
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
            UploadContext::Get().DrainPendingBinds();
            s_LastInspectorMaterialHandle  = matHandle;
            s_LastInspectorMaterialTexHash = texHash;
        }

        std::shared_ptr<Texture> albedo = material->GetTextureByType(MapType::Diffuse);
        material->UpdateGPUData();   // refresh m_GPUData (bindless indices + flags) before the preview UBO fill
        return RenderInspectorInternal(s_SphereModel, material->GetColor(), cam,
                                       ResolveBindlessIndex(albedo),
                                       Vec4(material->GetEmissiveColor(), material->GetEmissiveStrength()),
                                       material.get());
    }

    u32 GetInspectorSize() { return kInspectorSize; }
}
