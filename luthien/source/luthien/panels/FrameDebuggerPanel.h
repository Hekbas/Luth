#pragma once

#include "luthien/Editor.h"
#include "luth/scene/systems/RenderingSystem.h"
#include "luth/renderer/rendergraph/FrameEventTree.h"

#include <memory>

namespace Luth
{
    // FrameDebugger pass tree, archive previews, and per-draw replay all run against
    // descriptor sets allocated on the main thread (ImGui_ImplVulkan_AddTexture is not
    // MT-safe). Snapshot is a placeholder; the bulk of OnDraw remains inline because
    // moving descriptor management would require ThumbnailCache's bind-queue pattern.
    struct FrameDebuggerSnapshot { /* placeholder */ };

    class FrameDebuggerPanel : public Panel
    {
    public:
        FrameDebuggerPanel() { m_WindowID = "Frame Debugger"; }

        void OnInit() override;
        void OnGather(EditorSnapshotBuilder& builder) override;
        void OnDraw(const EditorSnapshot& snapshot) override;

        // Unity-style viewport pass preview. ScenePanel / GamePanel query
        // each frame; the overlay target is coupled to FrameDebugger's
        // capturedSource (stamped at capture time so a mid-Frozen source
        // toggle doesn't redirect the live overlay).
        struct OverlaySource
        {
            VkImageView view    = VK_NULL_HANDLE;
            VkSampler   sampler = VK_NULL_HANDLE;
            u32         width   = 0;
            u32         height  = 0;
        };

        bool ShouldOverlayInScene() const;
        bool ShouldOverlayInGame()  const;

        // Resolves the current selection into a view+sampler for ImGui::Image.
        // Draw selections trigger per-draw replay (preview takes precedence
        // when valid); color archives return their view directly; depth
        // archives route through BlitArchivedDepthToPreview for tonemapping.
        // Non-const because depth-blit + replay mutate FrameDebuggerContext.
        OverlaySource GetOverlaySource();

    private:
        // Live mode — pass-level view over the current graph snapshot.
        void DrawLiveView(const RG::RenderGraphSnapshot& snapshot);
        void DrawLiveControlBar(const RG::RenderGraphSnapshot& snapshot, int nonCulledCount);
        void DrawLivePassTree(const RG::RenderGraphSnapshot& snapshot);
        void DrawLivePassDetails(const RG::RenderGraphSnapshot& snapshot);

        // Capture mode — hierarchical EventNode tree over a frozen capture.
        void DrawCaptureView(const RG::CapturedFrame& capture);
        void DrawCaptureControlBar(const RG::CapturedFrame& capture);
        void DrawEventNode(const RG::CapturedFrame& capture, const RG::EventNode& node, int depthCounter);
        void DrawCaptureDetails(const RG::CapturedFrame& capture);
        void DrawArchivePreview(const RG::CapturedFrame& capture);
        void DrawSelectedDetailTables(const RG::CapturedFrame& capture);

        void SelectEventNode(const RG::EventNode& node);

        RenderingSystem* m_RS = nullptr;

        int  m_SelectedPassIndex     = -1;
        int  m_SelectedResourceIndex = -1;
        int  m_EventSliderValue      = -1;

        int  m_SelPassIndex     = -1;   // pass containing the selection (-1 = none)
        int  m_SelDrawIndex     = -1;   // global draw index (-1 if Pass/Cascade/Group selected)
        int  m_SelArchiveIdx    = -1;   // archive displayed in the Output preview
        int  m_SelArchiveLayer  = -1;   // -1 = whole image; else 2D-array slice (cascade)
        RG::EventNodeKind m_SelKind = RG::EventNodeKind::Group;

        // Descriptor caches keyed by VkImageView pointer, not archive index: a
        // recapture rebuilds the archive vector so indices can alias across
        // captures, but view pointers are unique per frame.
        VkImageView     m_DisplayArchiveViewCached = VK_NULL_HANDLE;
        VkDescriptorSet m_DisplayArchiveDescSet    = VK_NULL_HANDLE;

        VkImageView      m_PerDrawPreviewViewCached = VK_NULL_HANDLE;
        VkDescriptorSet  m_PerDrawPreviewDescSet    = VK_NULL_HANDLE;

        VkImageView      m_DepthPreviewViewCached   = VK_NULL_HANDLE;
        VkDescriptorSet  m_DepthPreviewDescSet      = VK_NULL_HANDLE;

    };
}
