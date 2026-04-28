#pragma once

#include "luthien/Editor.h"
#include "luth/scene/systems/RenderingSystem.h"
#include "luth/renderer/rendergraph/FrameEventTree.h"

#include <memory>

namespace Luth
{
    class FrameDebuggerPanel : public Panel
    {
    public:
        void OnInit() override;
        void OnRender() override;

        // Unity-style viewport pass preview. ScenePanel / GamePanel query the
        // panel each frame to decide whether to render the live LDR or the
        // currently-selected pass's archived RT in their viewport. The overlay
        // target is coupled to the capture source (set in the panel's control
        // bar): if the user captured Scene, the Scene viewport gets the
        // overlay; if Game, the Game viewport. Decided by FrameDebugger's
        // capturedSource (snapshotted at capture time) so toggling the source
        // mid-Frozen doesn't redirect the overlay to a viewport whose camera
        // never produced these archives.
        struct OverlaySource
        {
            VkImageView view    = VK_NULL_HANDLE;
            VkSampler   sampler = VK_NULL_HANDLE;
            u32         width   = 0;
            u32         height  = 0;
        };

        bool ShouldOverlayInScene() const;
        bool ShouldOverlayInGame()  const;

        // Resolves the currently-selected pass's primary archive into a view +
        // sampler suitable for ImGui::Image. Returns a null view when no valid
        // non-depth color archive is selected (depth archives are rendered via
        // the panel's own DrawArchivePreview path; viewport overlay falls back
        // to live rendering for those).
        OverlaySource GetOverlaySource() const;

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
