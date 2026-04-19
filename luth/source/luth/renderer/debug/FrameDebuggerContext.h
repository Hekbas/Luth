#pragma once

#include "luth/core/types/LuthTypes.h"
#include "luth/renderer/rendergraph/RenderGraph.h"

#include <vulkan/vulkan.h>
#include <vma/vk_mem_alloc.h>

namespace Luth
{
    class RenderPipeline;

    // Owns the render-side frame-debugger infrastructure that lives next to
    // RenderPipeline — per-draw + depth preview textures, the debug-blit
    // render-graph pass, and the replay-then-copy path. Distinct from
    // RenderingSystem::m_FrameDebugger, which holds the archive / state
    // machine / capture metadata.
    //
    // Constructed by RenderPipeline::Initialize; owns 12 preview-texture
    // fields (image/view/alloc/width/height/key per preview) and delegates
    // pipeline-level access via a RenderPipeline& friend.
    class FrameDebuggerContext
    {
    public:
        explicit FrameDebuggerContext(RenderPipeline& pipeline);
        ~FrameDebuggerContext();

        // Tear down the two preview textures. Called from RenderPipeline::Shutdown
        // before the Vulkan device is destroyed.
        void Shutdown();

        // Lazily create the debug-blit shader + descriptor resources. Safe to
        // call repeatedly; returns early once already initialised. Called
        // from Execute's capture branch and from BlitArchivedDepthToPreview.
        void InitDebugBlitResources();

        // Adds a final blit pass that copies `inputHandle` into LDROutput.
        // Used when scrubbing stops before PostProcess executes — without it
        // the HDR SceneColor (or depth ShadowMap) would never reach ImGui.
        RG::ResourceHandle AddDebugBlitPass(RG::RenderGraph& rg, RG::ResourceHandle inputHandle, bool isDepth);

        // Phase 14E — per-draw replay-then-copy. Re-executes GeometryPass up to
        // draw `localDrawIdx` (inclusive) into SceneColor, then copies the
        // result into the per-draw preview texture for ImGui sampling.
        void ReplayPassUpToDraw(u32 passIdx, u32 localDrawIdx);

        // Phase 14F — blit a cascade slice (or full depth archive) through the
        // tonemapping depth shader into the depth preview texture.
        void BlitArchivedDepthToPreview(u32 archiveIdx, int layer, float nearZ, float farZ);

        // Editor/debug accessors (forwarded by RenderPipeline).
        VkImageView GetPerDrawPreviewView()  const { return m_PerDrawPreviewView; }
        u64         GetPerDrawPreviewKey()   const { return m_PerDrawPreviewKey; }
        u32         GetPerDrawPreviewWidth() const { return m_PerDrawPreviewWidth; }
        u32         GetPerDrawPreviewHeight()const { return m_PerDrawPreviewHeight; }
        VkImageView GetDepthPreviewView()    const { return m_DepthPreviewView; }
        u32         GetDepthPreviewWidth()   const { return m_DepthPreviewWidth; }
        u32         GetDepthPreviewHeight()  const { return m_DepthPreviewHeight; }
        void        ResetPreviewCacheKeys() { m_PerDrawPreviewKey = UINT64_MAX; m_DepthPreviewKey = UINT64_MAX; }

    private:
        void EnsurePerDrawPreviewTexture(u32 width, u32 height);
        void DestroyPerDrawPreviewTexture();
        void EnsureDepthPreviewTexture(u32 width, u32 height);
        void DestroyDepthPreviewTexture();

        RenderPipeline& m_Pipeline;

        // Per-draw replay preview (matches SceneColor format, RGBA16F)
        VkImage       m_PerDrawPreviewImage  = VK_NULL_HANDLE;
        VkImageView   m_PerDrawPreviewView   = VK_NULL_HANDLE;
        VmaAllocation m_PerDrawPreviewAlloc  = nullptr;
        u32           m_PerDrawPreviewWidth  = 0;
        u32           m_PerDrawPreviewHeight = 0;
        u64           m_PerDrawPreviewKey    = UINT64_MAX;

        // Depth archive visualization preview (RGBA8 tonemapped)
        VkImage       m_DepthPreviewImage  = VK_NULL_HANDLE;
        VkImageView   m_DepthPreviewView   = VK_NULL_HANDLE;
        VmaAllocation m_DepthPreviewAlloc  = nullptr;
        u32           m_DepthPreviewWidth  = 0;
        u32           m_DepthPreviewHeight = 0;
        u64           m_DepthPreviewKey    = UINT64_MAX;
    };
}
