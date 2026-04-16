#pragma once

#include "luth/editor/Editor.h"
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

    private:
        // Live mode (Inactive state) — pass-level view (unchanged from Phase 9)
        void DrawLiveView(const RG::RenderGraphSnapshot& snapshot);
        void DrawLiveControlBar(const RG::RenderGraphSnapshot& snapshot, int nonCulledCount);
        void DrawLivePassTree(const RG::RenderGraphSnapshot& snapshot);
        void DrawLivePassDetails(const RG::RenderGraphSnapshot& snapshot);

        // Capture mode (Frozen state) — Phase 14D hierarchical EventNode tree
        void DrawCaptureView(const RG::CapturedFrame& capture);
        void DrawCaptureControlBar(const RG::CapturedFrame& capture);
        void DrawEventNode(const RG::CapturedFrame& capture, const RG::EventNode& node, int depthCounter);
        void DrawCaptureDetails(const RG::CapturedFrame& capture);
        void DrawArchivePreview(const RG::CapturedFrame& capture);
        void DrawSelectedDetailTables(const RG::CapturedFrame& capture);

        // Update Pass/Draw/Cascade selection when the user clicks a node.
        void SelectEventNode(const RG::EventNode& node);

        std::shared_ptr<RenderingSystem> m_RS;

        // Live mode state
        int  m_SelectedPassIndex     = -1;
        int  m_SelectedResourceIndex = -1;
        int  m_EventSliderValue      = -1;

        // Capture mode state (Phase 14D — tree-driven selection)
        int  m_SelPassIndex     = -1;   // pass containing the selection (-1 = none)
        int  m_SelDrawIndex     = -1;   // global draw index (-1 if Pass/Cascade/Group selected)
        int  m_SelArchiveIdx    = -1;   // archive to display in the Output preview
        int  m_SelArchiveLayer  = -1;   // -1 = whole image; else 2D-array slice (Cascade)
        RG::EventNodeKind m_SelKind = RG::EventNodeKind::Group;

        // Cached ImGui descriptor set for the currently-displayed color archive,
        // keyed against m_SelArchiveIdx so re-selecting the same archive doesn't
        // leak a new VkDescriptorSet. Created via ImGui_ImplVulkan_AddTexture.
        int             m_DisplayArchiveIdxCached = -1;
        VkDescriptorSet m_DisplayArchiveDescSet   = VK_NULL_HANDLE;

        // Tracks tree node uniqueness for ImGui::TreeNodeEx IDs across recursion.
        u32 m_TreeNodeCounter = 0;
    };
}
