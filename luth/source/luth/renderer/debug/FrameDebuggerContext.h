#pragma once

#include "luth/core/types/LuthTypes.h"
#include "luth/renderer/rendergraph/RenderGraph.h"

#include <vulkan/vulkan.h>
#include <vma/vk_mem_alloc.h>

namespace Luth
{
    class RenderPipeline;

    // Render-side frame-debugger infrastructure that lives next to RenderPipeline. Owns the
    // per-draw and depth preview textures, the debug-blit render-graph pass, and the replay-
    // then-copy path. Distinct from RenderingSystem::m_FrameDebugger, which holds the archive,
    // state machine, and capture metadata; this context only deals with the render-side preview
    // surface that the editor's Frame Debugger panel samples.
    //
    // Constructed by RenderPipeline::Initialize. Holds 12 preview-texture fields (image, view,
    // alloc, width, height, key per preview) and reaches back into RenderPipeline through its
    // public accessors.
    class FrameDebuggerContext
    {
    public:
        explicit FrameDebuggerContext(RenderPipeline& pipeline);
        ~FrameDebuggerContext();

        // Tear down the preview textures. Called from RenderPipeline::Shutdown before the Vulkan device is destroyed.
        void Shutdown();

        // Lazily create the debug-blit shader + descriptor resources. Safe to
        // call repeatedly; returns early once already initialised. Called
        // from Execute's capture branch and from BlitArchivedDepthToPreview.
        void InitDebugBlitResources();

        // Adds a final blit pass that copies `inputHandle` into LDROutput.
        // Used when scrubbing stops before PostProcess executes — without it
        // the HDR SceneColor (or depth ShadowMap) would never reach ImGui.
        RG::ResourceHandle AddDebugBlitPass(RG::RenderGraph& rg, RG::ResourceHandle inputHandle, bool isDepth);

        // Per-draw replay then copy. Re-executes the captured pass up to draw `localDrawIdx`
        // (inclusive) into a per-draw preview texture for ImGui sampling. Dispatches by pass name
        // to per-pass replay helpers (Geometry, Shadow, DepthPrepass, SelectionMask). Unsupported
        // passes leave m_PerDrawPreviewKey at its prior value; callers compare the returned key
        // against the requested one and fall back to the pass archive on mismatch.
        void ReplayPassUpToDraw(u32 passIdx, u32 localDrawIdx);

        // Blit a cascade slice (or full depth archive) through the tonemapping depth shader into
        // the depth preview texture for ImGui display.
        void BlitArchivedDepthToPreview(u32 archiveIdx, int layer, float nearZ, float farZ);

        // Blit a slim G-buffer archive through the appropriate decoder shader. Mode selects
        // SlimNormal (0) / SlimMotion (1) / SlimRoughness (2) / SlimMaterialID (3). scale is
        // the motion magnification (only used in mode 1; ignored otherwise).
        void BlitArchivedSlimToPreview(u32 archiveIdx, u32 mode, float scale);

        // Editor/debug accessors (forwarded by RenderPipeline).
        VkImageView GetPerDrawPreviewView()  const { return m_PerDrawPreviewView; }
        u64         GetPerDrawPreviewKey()   const { return m_PerDrawPreviewKey; }
        u32         GetPerDrawPreviewWidth() const { return m_PerDrawPreviewWidth; }
        u32         GetPerDrawPreviewHeight()const { return m_PerDrawPreviewHeight; }
        VkImageView GetDepthPreviewView()    const { return m_DepthPreviewView; }
        u32         GetDepthPreviewWidth()   const { return m_DepthPreviewWidth; }
        u32         GetDepthPreviewHeight()  const { return m_DepthPreviewHeight; }
        VkImageView GetSlimPreviewView()     const { return m_SlimPreviewView; }
        u32         GetSlimPreviewWidth()    const { return m_SlimPreviewWidth; }
        u32         GetSlimPreviewHeight()   const { return m_SlimPreviewHeight; }
        void        ResetPreviewCacheKeys() { m_PerDrawPreviewKey = UINT64_MAX; m_DepthPreviewKey = UINT64_MAX; m_SlimPreviewKey = UINT64_MAX; }

    private:
        void EnsurePerDrawPreviewTexture(u32 width, u32 height);
        void DestroyPerDrawPreviewTexture();
        void EnsureDepthPreviewTexture(u32 width, u32 height);
        void DestroyDepthPreviewTexture();
        void EnsureSlimPreviewTexture(u32 width, u32 height);
        void DestroySlimPreviewTexture();

        // Validate the captured view's FrameTargets is still alive (panel not closed)
        // and matches the identity stamped at FinalizeCapture. On mismatch, auto-exits
        // capture (state→Inactive, archives freed) and surfaces a notice via EditorHooks.
        // Returns true if the captured view is still usable.
        bool ValidateCapturedView();

        // Per-pass replay helpers. Each renders draws[0..localDrawIdx] into a
        // pass-appropriate target then blits/copies into m_PerDrawPreviewImage
        // and updates m_PerDrawPreviewKey on success. Unsupported pass names
        // leave the preview untouched; the caller falls back to pass-archive.
        void ReplayGeometry      (u32 passIdx, u32 localDrawIdx);
        void ReplayShadow        (u32 passIdx, u32 localDrawIdx, int cascadeIdx);
        void ReplayDepthPrepass  (u32 passIdx, u32 localDrawIdx);
        void ReplaySelectionMask (u32 passIdx, u32 localDrawIdx);

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

        // Slim G-buffer decoder preview (RGBA8 — shared by all 4 slim attachments via mode push).
        // Cache key = (archiveIdx << 32) | (mode << 16) | scaleBucket, so flipping between modes
        // for the same archive re-decodes lazily without churning the texture allocation.
        VkImage       m_SlimPreviewImage  = VK_NULL_HANDLE;
        VkImageView   m_SlimPreviewView   = VK_NULL_HANDLE;
        VmaAllocation m_SlimPreviewAlloc  = nullptr;
        u32           m_SlimPreviewWidth  = 0;
        u32           m_SlimPreviewHeight = 0;
        u64           m_SlimPreviewKey    = UINT64_MAX;
    };
}
