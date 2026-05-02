#include "lepch.h"
#include "luthien/panels/FrameDebuggerPanel.h"
#include "luthien/EditorSnapshot.h"
#include "luth/scene/systems/SystemRegistry.h"
#include "luthien/widgets/Widgets.h"
#include "luthien/widgets/Icons.h"
#include "luth/renderer/backend/vulkan/VulkanContext.h"

#include <vulkan/vulkan.h>
#include <backends/imgui_impl_vulkan.h>

namespace Luth
{
    // ---- Helpers ----

    static const char* FormatToString(RG::TextureFormat fmt)
    {
        switch (fmt)
        {
            case RG::TextureFormat::RGBA8_Unorm:        return "RGBA8";
            case RG::TextureFormat::BGRA8_Unorm:        return "BGRA8";
            case RG::TextureFormat::RGBA16_Float:       return "RGBA16F";
            case RG::TextureFormat::D32_Float:          return "D32F";
            case RG::TextureFormat::D24_Unorm_S8_Uint:  return "D24S8";
            default:                                    return "Unknown";
        }
    }

    // Walks the event tree depth-first, returning the Draw node whose
    // drawIndex matches `drawIdx`, else nullptr. Used by the draw-scrub
    // slider so SelectEventNode picks up the same archive/layer the tree
    // already resolved at finalize.
    static const RG::EventNode* FindDrawEventNode(const RG::EventNode& node, u32 drawIdx)
    {
        if (node.kind == RG::EventNodeKind::Draw && node.drawIndex == drawIdx)
            return &node;
        for (const auto& child : node.children)
            if (const RG::EventNode* found = FindDrawEventNode(child, drawIdx))
                return found;
        return nullptr;
    }

    // Capture-source dropdown shared between live + capture control bars.
    // Placed right of Enable / Disable, before any slider, so its position
    // stays stable across the live↔capture transition.
    static void DrawCaptureSourceCombo(RenderingSystem* rs)
    {
        static const char* k_Labels[] = { "Scene", "Game" };
        int srcIdx = (int)rs->GetCaptureSource();
        ImGui::SetNextItemWidth(80.0f);
        if (ImGui::Combo("##CaptureSource", &srcIdx, k_Labels, IM_ARRAYSIZE(k_Labels)))
            rs->SetCaptureSource((CaptureSource)srcIdx);
    }

    static bool IsDepthFormat(RG::TextureFormat fmt)
    {
        return fmt == RG::TextureFormat::D32_Float || fmt == RG::TextureFormat::D24_Unorm_S8_Uint;
    }

    static const char* CullModeToString(u32 mode)
    {
        switch (mode)
        {
            case VK_CULL_MODE_NONE:      return "None";
            case VK_CULL_MODE_FRONT_BIT: return "Front";
            case VK_CULL_MODE_BACK_BIT:  return "Back";
            default:                     return "Both";
        }
    }

    static const char* PolygonModeToString(VkPolygonMode mode)
    {
        switch (mode)
        {
            case VK_POLYGON_MODE_FILL:  return "Fill";
            case VK_POLYGON_MODE_LINE:  return "Line";
            case VK_POLYGON_MODE_POINT: return "Point";
            default:                    return "Unknown";
        }
    }

    static const char* RenderModeToString(u32 mode)
    {
        switch (mode)
        {
            case 0:  return "Opaque";
            case 1:  return "Cutout";
            case 2:  return "Transparent";
            case 3:  return "Fade";
            default: return "Unknown";
        }
    }

    // Map a non-culled pass slider index to its real index in snapshot.passes[]
    static int SliderToPassIndex(const RG::RenderGraphSnapshot& snapshot, int sliderVal)
    {
        int counter = 0;
        for (int i = 0; i < (int)snapshot.passes.size(); i++)
        {
            if (snapshot.passes[i].culled) continue;
            if (counter == sliderVal) return i;
            counter++;
        }
        return -1;
    }

    // ---- Init ----

    void FrameDebuggerPanel::OnInit()
    {
        m_RS = SystemRegistry::GetSystem<RenderingSystem>();
    }

    // ---- Public overlay accessors (Unity-style viewport preview) ----

    bool FrameDebuggerPanel::ShouldOverlayInScene() const
    {
        return m_RS
            && m_RS->GetDebuggerState() == DebuggerState::Frozen
            && m_RS->GetCapturedSource() == CaptureSource::Scene;
    }

    bool FrameDebuggerPanel::ShouldOverlayInGame() const
    {
        return m_RS
            && m_RS->GetDebuggerState() == DebuggerState::Frozen
            && m_RS->GetCapturedSource() == CaptureSource::Game;
    }

    FrameDebuggerPanel::OverlaySource FrameDebuggerPanel::GetOverlaySource()
    {
        OverlaySource src{};
        if (!m_RS) return src;
        if (m_RS->GetDebuggerState() != DebuggerState::Frozen) return src;

        const auto& capture = m_RS->GetCapturedFrame();
        if (!capture.valid) return src;

        // Per-draw replay path takes precedence for Draw selections so the
        // viewport overlay tracks the slider scrub (Unity behaviour). Replay
        // dispatch is internal; supported passes update the preview key and
        // we route the per-draw preview to the viewport. Unsupported passes
        // (compute, single-draw, ImGui) leave the key untouched and we fall
        // through to the pass-archive path below.
        if (m_SelKind == RG::EventNodeKind::Draw && m_SelPassIndex >= 0
            && m_SelPassIndex < (int)capture.passes.size() && m_SelDrawIndex >= 0)
        {
            const auto& pass = capture.passes[m_SelPassIndex];
            if ((u32)m_SelDrawIndex >= pass.firstDrawIndex
                && (u32)m_SelDrawIndex < pass.firstDrawIndex + pass.drawCallCount)
            {
                const u32 perDrawPassIdx  = (u32)m_SelPassIndex;
                const u32 perDrawLocalIdx = (u32)m_SelDrawIndex - pass.firstDrawIndex;
                m_RS->ReplayPassUpToDraw(perDrawPassIdx, perDrawLocalIdx);

                const u64 expectedKey  = ((u64)perDrawPassIdx << 32) | (u64)perDrawLocalIdx;
                const bool replayValid = (m_RS->GetPerDrawPreviewKey() == expectedKey);
                if (replayValid)
                {
                    VkImageView previewView = m_RS->GetPerDrawPreviewView();
                    const u32   pw          = m_RS->GetPerDrawPreviewWidth();
                    const u32   ph          = m_RS->GetPerDrawPreviewHeight();
                    if (previewView != VK_NULL_HANDLE && pw > 0 && ph > 0)
                    {
                        src.view    = previewView;
                        src.sampler = m_RS->GetDebugSampler();
                        src.width   = pw;
                        src.height  = ph;
                        return src;
                    }
                }
            }
        }

        if (m_SelArchiveIdx < 0 || m_SelArchiveIdx >= (int)capture.archivedImages.size()) return src;

        const auto& archive = capture.archivedImages[m_SelArchiveIdx];

        if (archive.isDepth)
        {
            // Depth archives go through BlitArchivedDepthToPreview which
            // tonemaps the depth into an RGBA8 preview texture. Cascade
            // slices use the matching cascade's view-Z far split so each
            // slice gets a sensible contrast range; non-cascade depth uses
            // a generic 0.1..200 m window (matches the panel thumbnail).
            float nearZ = 0.1f;
            float farZ  = 200.0f;
            if (m_SelArchiveLayer >= 0 && m_SelArchiveLayer < 4)
            {
                farZ = capture.cascadeSplitsViewZ[m_SelArchiveLayer];
                if (m_SelArchiveLayer > 0)
                    nearZ = capture.cascadeSplitsViewZ[m_SelArchiveLayer - 1];
                if (farZ <= nearZ) farZ = nearZ + 1.0f;
            }

            m_RS->BlitArchivedDepthToPreview((u32)m_SelArchiveIdx, m_SelArchiveLayer, nearZ, farZ);

            VkImageView depthPreview = m_RS->GetDepthPreviewView();
            const u32   dpW          = m_RS->GetDepthPreviewWidth();
            const u32   dpH          = m_RS->GetDepthPreviewHeight();
            if (depthPreview == VK_NULL_HANDLE || dpW == 0 || dpH == 0) return src;

            src.view    = depthPreview;
            src.sampler = m_RS->GetDebugSampler();
            src.width   = dpW;
            src.height  = dpH;
            return src;
        }

        if (archive.view == VK_NULL_HANDLE) return src;

        src.view    = archive.view;
        src.sampler = m_RS->GetDebugSampler();
        src.width   = archive.width;
        src.height  = archive.height;
        return src;
    }

    // ---- Main Render ----

    void FrameDebuggerPanel::OnGather(EditorSnapshotBuilder& builder)
    {
        // Pass tree + archive previews + per-draw replay all need ImGui descriptor
        // allocations on main thread; snapshot stays a placeholder for v2.9.0.
        builder.Add<FrameDebuggerSnapshot>();
    }

    void FrameDebuggerPanel::OnDraw(const EditorSnapshot& /*snapshot*/)
    {
        LH_PROFILE_FUNCTION();
        if (!m_RS) return;

        ImGui::PushFont(Editor::GetFASolid());
        std::string title = ICON_FA_DIAGRAM_PROJECT + std::string("  Frame Debugger");
        BeginWindow(title.c_str());
        ImGui::PopFont();

        auto debuggerState = m_RS->GetDebuggerState();
        const bool inCaptureView = (debuggerState == DebuggerState::Frozen && m_RS->GetCapturedFrame().valid);

        // Phase 14D/E — release cached ImGui descriptors as soon as we leave
        // the capture view. The underlying ArchivedImage / per-draw-preview
        // views can be destroyed on ExitCapture / Resize, so stale cached
        // descriptors would point at freed GPU memory next frame.
        if (!inCaptureView)
        {
            auto dropDesc = [](VkDescriptorSet& set) {
                if (set == VK_NULL_HANDLE) return;
                VkDescriptorSet stale = set;
                VulkanContext::Get().PushDeletion([stale]() {
                    ImGui_ImplVulkan_RemoveTexture(stale);
                });
                set = VK_NULL_HANDLE;
            };
            dropDesc(m_DisplayArchiveDescSet);
            dropDesc(m_PerDrawPreviewDescSet);
            dropDesc(m_DepthPreviewDescSet);
            m_DisplayArchiveViewCached  = VK_NULL_HANDLE;
            m_PerDrawPreviewViewCached  = VK_NULL_HANDLE;
            m_DepthPreviewViewCached    = VK_NULL_HANDLE;
            m_SelKind          = RG::EventNodeKind::Group;
            m_SelPassIndex     = -1;
            m_SelDrawIndex     = -1;
            m_SelArchiveIdx    = -1;
            m_SelArchiveLayer  = -1;
        }

        if (inCaptureView)
            DrawCaptureView(m_RS->GetCapturedFrame());
        else
            DrawLiveView(m_RS->GetGraphSnapshot());

        ImGui::End();
    }

    // 25002500  Live Mode (pass-level, same as before) 25002500

    void FrameDebuggerPanel::DrawLiveView(const RG::RenderGraphSnapshot& snapshot)
    {
        if (snapshot.passes.empty())
        {
            ImGui::TextDisabled("No render graph data");
            return;
        }

        int nonCulledCount = 0;
        for (auto& p : snapshot.passes)
            if (!p.culled) nonCulledCount++;

        if (nonCulledCount > 0)
        {
            if (m_EventSliderValue < 0) m_EventSliderValue = 0;
            if (m_SelectedPassIndex < 0) m_SelectedPassIndex = SliderToPassIndex(snapshot, 0);
        }

        DrawLiveControlBar(snapshot, nonCulledCount);
        ImGui::Separator();

        float availWidth = ImGui::GetContentRegionAvail().x;
        float leftWidth = availWidth * 0.35f;
        if (leftWidth < 180.0f) leftWidth = 180.0f;

        ImGui::BeginChild("##PassTree", ImVec2(leftWidth, 0), ImGuiChildFlags_ResizeX | ImGuiChildFlags_Borders);
        DrawLivePassTree(snapshot);
        ImGui::EndChild();

        ImGui::SameLine();

        float rightWidth = ImGui::GetContentRegionAvail().x;
        if (rightWidth < 250.0f) rightWidth = 250.0f;
        ImGui::BeginChild("##PassDetails", ImVec2(rightWidth, 0), ImGuiChildFlags_Borders);
        DrawLivePassDetails(snapshot);
        ImGui::EndChild();
    }

    void FrameDebuggerPanel::DrawLiveControlBar(const RG::RenderGraphSnapshot& snapshot, int nonCulledCount)
    {
        // Enable button — triggers capture. Phase 14D — capture-mode selection
        // state is reset lazily on entering Frozen view (OnRender top-of-frame
        // guard); no explicit slider/draw-index reset needed here anymore.
        if (ImGui::Button("Enable"))
            m_RS->RequestCapture();

        ImGui::SameLine();
        DrawCaptureSourceCombo(m_RS);
        ImGui::SameLine();

        // Event slider
        float arrowsWidth = 60.0f;
        float timeWidth = 80.0f;
        float sliderWidth = ImGui::GetContentRegionAvail().x - arrowsWidth - timeWidth - ImGui::GetStyle().ItemSpacing.x * 3;
        if (sliderWidth < 80.0f) sliderWidth = 80.0f;

        ImGui::SetNextItemWidth(sliderWidth);
        if (ImGui::SliderInt("##EventSlider", &m_EventSliderValue, 0, nonCulledCount - 1))
        {
            m_SelectedPassIndex = SliderToPassIndex(snapshot, m_EventSliderValue);
            auto& pass = snapshot.passes[m_SelectedPassIndex];
            m_SelectedResourceIndex = pass.primaryOutputIndex;
        }

        ImGui::SameLine();

        if (ImGui::ArrowButton("##PrevPass", ImGuiDir_Left))
        {
            if (m_EventSliderValue > 0) {
                m_EventSliderValue--;
                m_SelectedPassIndex = SliderToPassIndex(snapshot, m_EventSliderValue);
                m_SelectedResourceIndex = snapshot.passes[m_SelectedPassIndex].primaryOutputIndex;
            }
        }
        ImGui::SameLine();
        if (ImGui::ArrowButton("##NextPass", ImGuiDir_Right))
        {
            if (m_EventSliderValue < nonCulledCount - 1) {
                m_EventSliderValue++;
                m_SelectedPassIndex = SliderToPassIndex(snapshot, m_EventSliderValue);
                m_SelectedResourceIndex = snapshot.passes[m_SelectedPassIndex].primaryOutputIndex;
            }
        }

        // Right-aligned total GPU time
        ImGui::SameLine();
        char totalBuf[32];
        if (snapshot.totalGpuTimeMs > 0.0f)
            snprintf(totalBuf, sizeof(totalBuf), "%.2f ms", snapshot.totalGpuTimeMs);
        else
            snprintf(totalBuf, sizeof(totalBuf), "-- ms");

        float textW = ImGui::CalcTextSize(totalBuf).x;
        float rightEdge = ImGui::GetWindowContentRegionMax().x;
        if (rightEdge - ImGui::GetCursorPosX() > textW)
            ImGui::SetCursorPosX(rightEdge - textW);
        ImGui::Text("%s", totalBuf);
    }

    void FrameDebuggerPanel::DrawLivePassTree(const RG::RenderGraphSnapshot& snapshot)
    {
        int nonCulledIdx = 0;
        for (int i = 0; i < (int)snapshot.passes.size(); i++)
        {
            auto& pass = snapshot.passes[i];
            if (pass.culled) continue;

            bool isSelected = (m_SelectedPassIndex == i);
            ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_SpanAvailWidth;
            if (isSelected) flags |= ImGuiTreeNodeFlags_Selected;

            ImGui::TreeNodeEx((void*)(intptr_t)i, flags, "%s", pass.name.c_str());

            if (ImGui::IsItemClicked(ImGuiMouseButton_Left))
            {
                m_SelectedPassIndex = i;
                m_EventSliderValue = nonCulledIdx;
                m_SelectedResourceIndex = pass.primaryOutputIndex;
            }

            // Right-aligned timing + draw count
            char infoBuf[64];
            if (pass.gpuTimeMs >= 0.0f)
            {
                if (pass.drawCalls > 0)
                    snprintf(infoBuf, sizeof(infoBuf), "%u  %.2f ms", pass.drawCalls, pass.gpuTimeMs);
                else
                    snprintf(infoBuf, sizeof(infoBuf), "%.2f ms", pass.gpuTimeMs);
            }
            else
                snprintf(infoBuf, sizeof(infoBuf), "--");

            float textWidth = ImGui::CalcTextSize(infoBuf).x;
            float availX = ImGui::GetWindowContentRegionMax().x;
            ImGui::SameLine(availX - textWidth);
            ImGui::TextDisabled("%s", infoBuf);

            nonCulledIdx++;
        }
    }

    void FrameDebuggerPanel::DrawLivePassDetails(const RG::RenderGraphSnapshot& snapshot)
    {
        if (m_SelectedPassIndex < 0 || m_SelectedPassIndex >= (int)snapshot.passes.size())
        {
            ImGui::TextDisabled("Select a render pass to view details");
            return;
        }

        auto& pass = snapshot.passes[m_SelectedPassIndex];

        // Output Preview
        if (UI::BeginCollapsingHeader("Output", true))
        {
            int previewIdx = m_SelectedResourceIndex;
            if (previewIdx < 0) previewIdx = pass.primaryOutputIndex;

            if (previewIdx >= 0 && previewIdx < (int)snapshot.resources.size())
            {
                auto& res = snapshot.resources[previewIdx];
                ImGui::Text("%s  (%ux%u %s)", res.name.c_str(), res.width, res.height, FormatToString(res.format));

                if (IsDepthFormat(res.format))
                    ImGui::TextDisabled("(depth buffer - no preview)");
                else
                {
                    auto tex = m_RS->GetNamedTexture(res.name);
                    if (tex)
                    {
                        float panelW = ImGui::GetContentRegionAvail().x;
                        float maxH = 300.0f;
                        float texW = (float)tex->GetWidth(), texH = (float)tex->GetHeight();
                        float ar = texW / texH;
                        float drawW = panelW, drawH = drawW / ar;
                        if (drawH > maxH) { drawH = maxH; drawW = drawH * ar; }
                        float offsetX = (panelW - drawW) * 0.5f;
                        if (offsetX > 0.0f) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + offsetX);
                        ImTextureID texID = UI::GetTextureID(tex);
                        ImGui::Image(texID, ImVec2(drawW, drawH), ImVec2(0, 0), ImVec2(1, 1));
                    }
                    else
                        ImGui::TextDisabled("(no preview available)");
                }
            }
            else
                ImGui::TextDisabled("No output resource");
            UI::EndCollapsingHeader();
        }

        ImGui::Spacing();

        // Details
        if (UI::BeginCollapsingHeader("Details", true))
        {
            ImGui::Indent(4.0f);

            if (pass.primaryOutputIndex >= 0 && pass.primaryOutputIndex < (int)snapshot.resources.size())
            {
                auto& res = snapshot.resources[pass.primaryOutputIndex];
                if (ImGui::BeginTable("##RTInfo", 2)) {
                    ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 130.0f);
                    ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
                    ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextDisabled("Render Target"); ImGui::TableNextColumn(); ImGui::Text("%s", res.name.c_str());
                    ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextDisabled("Size"); ImGui::TableNextColumn(); ImGui::Text("%ux%u", res.width, res.height);
                    ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextDisabled("Format"); ImGui::TableNextColumn(); ImGui::Text("%s", FormatToString(res.format));
                    ImGui::EndTable();
                }
            }

            ImGui::Spacing(); ImGui::Spacing();

            if (ImGui::BeginTable("##PipelineState", 2)) {
                ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 130.0f);
                ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextDisabled("Depth Test");  ImGui::TableNextColumn(); ImGui::Text("%s", pass.depthTest ? "On" : "Off");
                ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextDisabled("Depth Write"); ImGui::TableNextColumn(); ImGui::Text("%s", pass.depthWrite ? "On" : "Off");
                ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextDisabled("Blend");       ImGui::TableNextColumn(); ImGui::Text("%s", pass.blendEnabled ? "On" : "Off");
                ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextDisabled("Cull Mode");   ImGui::TableNextColumn(); ImGui::Text("%s", CullModeToString(pass.cullMode));
                ImGui::EndTable();
            }

            ImGui::Spacing(); ImGui::Spacing();

            if (ImGui::BeginTable("##GeoStats", 2)) {
                ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 130.0f);
                ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextDisabled("Draw Calls"); ImGui::TableNextColumn(); ImGui::Text("%u", pass.drawCalls);
                if (pass.indices > 0) { ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextDisabled("Indices"); ImGui::TableNextColumn(); ImGui::Text("%u", pass.indices); }
                ImGui::EndTable();
            }

            ImGui::Spacing(); ImGui::Spacing();

            if (ImGui::BeginTable("##ShaderInfo", 2)) {
                ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 130.0f);
                ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
                if (!pass.shaderName.empty()) { ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextDisabled("Shader"); ImGui::TableNextColumn(); ImGui::Text("%s", pass.shaderName.c_str()); }
                if (pass.gpuTimeMs >= 0.0f) { ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextDisabled("GPU Time"); ImGui::TableNextColumn(); ImGui::Text("%.3f ms", pass.gpuTimeMs); }
                ImGui::EndTable();
            }

            ImGui::Unindent(4.0f);
            UI::EndCollapsingHeader();
        }

        ImGui::Spacing();

        // Resources
        if (UI::BeginCollapsingHeader("Resources"))
        {
            ImGui::Indent(4.0f);
            if (!pass.reads.empty())
            {
                ImGui::TextDisabled("Reads:");
                ImGui::SameLine();
                for (size_t r = 0; r < pass.reads.size(); r++) {
                    if (r > 0) ImGui::SameLine();
                    if (ImGui::SmallButton(pass.reads[r].name.c_str()))
                        m_SelectedResourceIndex = (int)pass.reads[r].index - 1;
                }
            }
            if (!pass.writes.empty())
            {
                ImGui::TextDisabled("Writes:");
                ImGui::SameLine();
                for (size_t w = 0; w < pass.writes.size(); w++) {
                    if (w > 0) ImGui::SameLine();
                    if (ImGui::SmallButton(pass.writes[w].name.c_str()))
                        m_SelectedResourceIndex = (int)pass.writes[w].index - 1;
                }
            }
            ImGui::Unindent(4.0f);
            UI::EndCollapsingHeader();
        }
    }

    // 25002500  Capture Mode (Phase 14D — hierarchical EventNode tree) 25002500

    void FrameDebuggerPanel::DrawCaptureView(const RG::CapturedFrame& capture)
    {
        DrawCaptureControlBar(capture);
        ImGui::Separator();

        float availWidth = ImGui::GetContentRegionAvail().x;
        float leftWidth  = availWidth * 0.40f;
        if (leftWidth < 220.0f) leftWidth = 220.0f;

        ImGui::BeginChild("##EventTree", ImVec2(leftWidth, 0),
                          ImGuiChildFlags_ResizeX | ImGuiChildFlags_Borders);
        // Walk the root's children directly — the root itself is the implicit
        // "Frame" container and isn't drawn (matches Unity's tree shape).
        for (const auto& child : capture.rootEvent.children)
            DrawEventNode(capture, child, 0);
        ImGui::EndChild();

        ImGui::SameLine();

        float rightWidth = ImGui::GetContentRegionAvail().x;
        if (rightWidth < 280.0f) rightWidth = 280.0f;
        ImGui::BeginChild("##CaptureDetails", ImVec2(rightWidth, 0), ImGuiChildFlags_Borders);
        DrawCaptureDetails(capture);
        ImGui::EndChild();
    }

    void FrameDebuggerPanel::DrawCaptureControlBar(const RG::CapturedFrame& capture)
    {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.7f, 0.2f, 0.2f, 1.0f));
        if (ImGui::Button("Disable"))
        {
            // Clear selection before ExitCapture invalidates archive views; the
            // OnRender top-of-frame guard would otherwise see a stale descriptor.
            auto dropDesc = [](VkDescriptorSet& set) {
                if (set == VK_NULL_HANDLE) return;
                VkDescriptorSet stale = set;
                VulkanContext::Get().PushDeletion([stale]() {
                    ImGui_ImplVulkan_RemoveTexture(stale);
                });
                set = VK_NULL_HANDLE;
            };
            dropDesc(m_DisplayArchiveDescSet);
            dropDesc(m_PerDrawPreviewDescSet);
            dropDesc(m_DepthPreviewDescSet);
            m_DisplayArchiveViewCached  = VK_NULL_HANDLE;
            m_PerDrawPreviewViewCached  = VK_NULL_HANDLE;
            m_DepthPreviewViewCached    = VK_NULL_HANDLE;
            m_SelKind         = RG::EventNodeKind::Group;
            m_SelPassIndex    = -1;
            m_SelDrawIndex    = -1;
            m_SelArchiveIdx   = -1;
            m_SelArchiveLayer = -1;
            m_RS->ExitCapture();
            ImGui::PopStyleColor();
            return;
        }
        ImGui::PopStyleColor();

        ImGui::SameLine();
        // Capture source — same control + position as in the live bar so it
        // doesn't visually jump on the live↔capture transition. Mutating
        // here only affects the *next* capture; the active overlay is keyed
        // off capturedSource (snapshotted at finalize).
        DrawCaptureSourceCombo(m_RS);
        ImGui::SameLine();

        const int drawCount = (int)capture.drawCalls.size();
        if (drawCount > 0)
        {
            // Auto-select draw 0 the first frame the panel renders a fresh
            // capture (or whenever the user has cleared the selection by
            // clicking a Draw-less Group). Keeps the slider + viewport
            // overlay live without requiring a tree click.
            if (m_SelDrawIndex < 0 || m_SelDrawIndex >= drawCount)
            {
                if (const RG::EventNode* n = FindDrawEventNode(capture.rootEvent, 0))
                    SelectEventNode(*n);
            }

            int sliderValue = m_SelDrawIndex;
            if (sliderValue < 0)            sliderValue = 0;
            if (sliderValue >= drawCount)   sliderValue = drawCount - 1;

            // Slider format: "<PassName>: <MeshName> (<idx>)" — runtime-built
            // per position. snprintf with `%%d` produces `%d` in the format
            // string, which ImGui::SliderInt then expands with the int value.
            char fmt[224];
            if ((u32)sliderValue < capture.drawCalls.size())
            {
                const auto& dc = capture.drawCalls[sliderValue];
                snprintf(fmt, sizeof(fmt), "%s: %s (%%d)",
                         dc.passName.c_str(), dc.meshName.c_str());
            }
            else
            {
                snprintf(fmt, sizeof(fmt), "(invalid) (%%d)");
            }

            const float arrowsWidth = 60.0f;
            const float statsWidth  = 200.0f;
            const float spacing     = ImGui::GetStyle().ItemSpacing.x;
            float sliderWidth = ImGui::GetContentRegionAvail().x - arrowsWidth - statsWidth - spacing * 3;
            if (sliderWidth < 100.0f) sliderWidth = 100.0f;

            ImGui::SetNextItemWidth(sliderWidth);
            int newValue = sliderValue;
            if (ImGui::SliderInt("##DrawScrub", &newValue, 0, drawCount - 1, fmt))
            {
                if (const RG::EventNode* n = FindDrawEventNode(capture.rootEvent, (u32)newValue))
                    SelectEventNode(*n);
            }

            ImGui::SameLine();
            if (ImGui::ArrowButton("##PrevDraw", ImGuiDir_Left) && sliderValue > 0)
            {
                if (const RG::EventNode* n = FindDrawEventNode(capture.rootEvent, (u32)(sliderValue - 1)))
                    SelectEventNode(*n);
            }
            ImGui::SameLine();
            if (ImGui::ArrowButton("##NextDraw", ImGuiDir_Right) && sliderValue < drawCount - 1)
            {
                if (const RG::EventNode* n = FindDrawEventNode(capture.rootEvent, (u32)(sliderValue + 1)))
                    SelectEventNode(*n);
            }

            // [ / ] keyboard scrub. Gated on panel-window focus so the
            // shortcut doesn't fire while the user types in another panel.
            if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows))
            {
                if (ImGui::IsKeyPressed(ImGuiKey_LeftBracket, /*repeat*/true) && sliderValue > 0)
                {
                    if (const RG::EventNode* n = FindDrawEventNode(capture.rootEvent, (u32)(sliderValue - 1)))
                        SelectEventNode(*n);
                }
                if (ImGui::IsKeyPressed(ImGuiKey_RightBracket, /*repeat*/true) && sliderValue < drawCount - 1)
                {
                    if (const RG::EventNode* n = FindDrawEventNode(capture.rootEvent, (u32)(sliderValue + 1)))
                        SelectEventNode(*n);
                }
            }
        }

        ImGui::SameLine();
        char status[160];
        snprintf(status, sizeof(status),
                 "%zu passes | %zu draws | %.2f ms",
                 capture.passes.size(),
                 capture.drawCalls.size(),
                 capture.totalGpuTimeMs);
        ImGui::TextDisabled("%s", status);
    }

    void FrameDebuggerPanel::SelectEventNode(const RG::EventNode& node)
    {
        m_SelKind         = node.kind;
        m_SelPassIndex    = (node.passIndex == UINT32_MAX) ? -1 : (int)node.passIndex;
        m_SelArchiveIdx   = node.archivedImageIndex;
        m_SelArchiveLayer = node.archiveLayer;

        // Unity-style scrub semantics: clicking a Draw node lands on it
        // exactly; clicking a Group/Pass/Cascade snaps the slider to the
        // last leaf draw under that node (precomputed in BuildEventTree).
        // Draw-less subtrees (e.g. an empty Group) leave the slider where
        // it was so the user doesn't lose their scrub position.
        if (node.kind == RG::EventNodeKind::Draw)
            m_SelDrawIndex = (int)node.drawIndex;
        else if (node.lastDrawIndex != UINT32_MAX)
            m_SelDrawIndex = (int)node.lastDrawIndex;
    }

    void FrameDebuggerPanel::DrawEventNode(const RG::CapturedFrame& capture,
                                            const RG::EventNode& node,
                                            int /*depthCounter*/)
    {
        // --- Selection state for highlight ---
        bool isSelected = false;
        switch (node.kind)
        {
            case RG::EventNodeKind::Pass:
            case RG::EventNodeKind::Cascade:
                isSelected = (m_SelKind != RG::EventNodeKind::Group &&
                              m_SelPassIndex == (int)node.passIndex &&
                              m_SelDrawIndex == -1);
                break;
            case RG::EventNodeKind::Draw:
                isSelected = (m_SelKind == RG::EventNodeKind::Draw &&
                              m_SelDrawIndex == (int)node.drawIndex);
                break;
            case RG::EventNodeKind::Group:
                isSelected = false;  // groups are containers, not selectable targets
                break;
        }

        // --- Tree node flags ---
        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_OpenOnArrow;
        if (node.children.empty())
            flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
        if (isSelected)
            flags |= ImGuiTreeNodeFlags_Selected;
        if (node.kind == RG::EventNodeKind::Group ||
            (node.kind == RG::EventNodeKind::Pass && !node.children.empty()))
            flags |= ImGuiTreeNodeFlags_DefaultOpen;

        // --- Label ---
        char label[256];
        if (node.kind == RG::EventNodeKind::Pass &&
            node.passIndex < capture.passes.size())
        {
            snprintf(label, sizeof(label), "%s  (%u)",
                     node.label.c_str(),
                     capture.passes[node.passIndex].drawCallCount);
        }
        else
        {
            snprintf(label, sizeof(label), "%s", node.label.c_str());
        }

        // Stable per-node ID for ImGui's open/closed state map. A counter
        // would collide across re-renders if the tree structure shifted (e.g.
        // a Group appears/disappears between captures); ImGui then carries
        // open-state across to the wrong node, producing the cross-talk
        // surfaced in B verification. Key on node identity instead.
        // Top 4 bits = kind tag, low 60 bits = payload.
        u64 stableId = 0;
        switch (node.kind)
        {
            case RG::EventNodeKind::Group:
                stableId = ((u64)1 << 60) | (std::hash<std::string>{}(node.label) & 0x0FFFFFFFFFFFFFFFull);
                break;
            case RG::EventNodeKind::Pass:
                stableId = ((u64)2 << 60) | node.passIndex;
                break;
            case RG::EventNodeKind::Cascade:
                stableId = ((u64)3 << 60) | node.passIndex;
                break;
            case RG::EventNodeKind::Draw:
                stableId = ((u64)4 << 60) | node.drawIndex;
                break;
        }
        bool nodeOpen = ImGui::TreeNodeEx((void*)(uintptr_t)stableId, flags, "%s", label);

        // CRITICAL: capture click status BEFORE any subsequent ImGui call —
        // ImGui::IsItemClicked refers to the most recently submitted item, and
        // the right-aligned TextDisabled below would shift it to a non-clickable
        // disabled label, swallowing all selection clicks (incl. Draw nodes).
        const bool clickedThisNode = ImGui::IsItemClicked(ImGuiMouseButton_Left);

        // --- Right-aligned annotation: GPU time for passes, idx count for draws ---
        if ((node.kind == RG::EventNodeKind::Pass || node.kind == RG::EventNodeKind::Cascade)
            && node.gpuTimeMs >= 0.0f)
        {
            char buf[32];
            snprintf(buf, sizeof(buf), "%.2f ms", node.gpuTimeMs);
            float tw    = ImGui::CalcTextSize(buf).x;
            float avail = ImGui::GetWindowContentRegionMax().x;
            ImGui::SameLine(avail - tw);
            ImGui::TextDisabled("%s", buf);
        }
        else if (node.kind == RG::EventNodeKind::Draw &&
                 node.drawIndex < capture.drawCalls.size())
        {
            const auto& dc = capture.drawCalls[node.drawIndex];
            if (dc.indexCount > 0)
            {
                char buf[32];
                snprintf(buf, sizeof(buf), "%u idx", dc.indexCount);
                float tw    = ImGui::CalcTextSize(buf).x;
                float avail = ImGui::GetWindowContentRegionMax().x;
                ImGui::SameLine(avail - tw);
                ImGui::TextDisabled("%s", buf);
            }
        }

        // --- Click handler (groups select nothing — they only expand) ---
        if (clickedThisNode && node.kind != RG::EventNodeKind::Group)
            SelectEventNode(node);

        // --- Recurse into children if open and not a leaf ---
        if (nodeOpen && !node.children.empty() && !(flags & ImGuiTreeNodeFlags_NoTreePushOnOpen))
        {
            for (const auto& child : node.children)
                DrawEventNode(capture, child, 0);
            ImGui::TreePop();
        }
    }

    void FrameDebuggerPanel::DrawCaptureDetails(const RG::CapturedFrame& capture)
    {
        if (m_SelKind == RG::EventNodeKind::Group)
        {
            ImGui::TextDisabled("Select a pass or draw call to view details");
            return;
        }

        DrawArchivePreview(capture);
        ImGui::Spacing();
        DrawSelectedDetailTables(capture);
    }

    void FrameDebuggerPanel::DrawArchivePreview(const RG::CapturedFrame& capture)
    {
        if (!UI::BeginCollapsingHeader("Output", true))
            return;

        VkSampler sampler = m_RS->GetDebugSampler();

        // -------------------------------------------------------------------
        // Phase 14E — per-draw replay preview.
        //
        // Triggered only when a Draw node is selected AND its owning pass is
        // one we know how to replay (GeometryPass for v1). Other selections
        // fall through to the pass-output archive path below.
        // -------------------------------------------------------------------
        bool tryPerDrawReplay = false;
        u32  perDrawPassIdx   = 0;
        u32  perDrawLocalIdx  = 0;

        if (m_SelKind == RG::EventNodeKind::Draw && m_SelPassIndex >= 0 &&
            m_SelPassIndex < (int)capture.passes.size() && m_SelDrawIndex >= 0)
        {
            // Attempt per-draw replay for any pass; ReplayPassUpToDraw
            // dispatches by pass.name internally and no-ops for unsupported
            // pass types. Validity is gated below by comparing the post-
            // replay preview key against what we requested — supported
            // passes update the key, unsupported ones leave it untouched.
            const auto& pass = capture.passes[m_SelPassIndex];
            if ((u32)m_SelDrawIndex >= pass.firstDrawIndex &&
                (u32)m_SelDrawIndex <  pass.firstDrawIndex + pass.drawCallCount)
            {
                tryPerDrawReplay = true;
                perDrawPassIdx   = (u32)m_SelPassIndex;
                perDrawLocalIdx  = (u32)m_SelDrawIndex - pass.firstDrawIndex;
            }
        }

        if (tryPerDrawReplay && sampler != VK_NULL_HANDLE)
        {
            // RenderingSystem caches by (passIdx,localDrawIdx); identical
            // re-selections short-circuit to a no-op inside ReplayPassUpToDraw.
            m_RS->ReplayPassUpToDraw(perDrawPassIdx, perDrawLocalIdx);

            VkImageView previewView = m_RS->GetPerDrawPreviewView();
            u32         previewW    = m_RS->GetPerDrawPreviewWidth();
            u32         previewH    = m_RS->GetPerDrawPreviewHeight();

            // Replay is "valid" iff its key matches what we requested.
            // Unsupported pass types leave the key unchanged, in which case
            // `previewView` reflects an unrelated prior replay — fall through
            // to the pass-archive path so the user sees a coherent image.
            const u64 expectedKey  = ((u64)perDrawPassIdx << 32) | (u64)perDrawLocalIdx;
            const bool replayValid = (m_RS->GetPerDrawPreviewKey() == expectedKey);

            if (replayValid && previewView != VK_NULL_HANDLE && previewW > 0 && previewH > 0)
            {
                ImGui::Text("Per-draw replay  (%ux%u, draw %u of %u)",
                            previewW, previewH,
                            perDrawLocalIdx + 1,
                            capture.passes[perDrawPassIdx].drawCallCount);

                // (Re)allocate the ImGui descriptor when the underlying view
                // pointer changes (e.g. preview resized on viewport change).
                if (previewView != m_PerDrawPreviewViewCached)
                {
                    if (m_PerDrawPreviewDescSet != VK_NULL_HANDLE)
                    {
                        VkDescriptorSet stale = m_PerDrawPreviewDescSet;
                        VulkanContext::Get().PushDeletion([stale]() {
                            ImGui_ImplVulkan_RemoveTexture(stale);
                        });
                    }
                    m_PerDrawPreviewDescSet = ImGui_ImplVulkan_AddTexture(
                        sampler, previewView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                    m_PerDrawPreviewViewCached = previewView;
                }

                if (m_PerDrawPreviewDescSet != VK_NULL_HANDLE)
                {
                    float panelW = ImGui::GetContentRegionAvail().x;
                    float maxH   = 320.0f;
                    float ar     = (float)previewW / (float)previewH;
                    float drawW  = panelW;
                    float drawH  = drawW / ar;
                    if (drawH > maxH) { drawH = maxH; drawW = drawH * ar; }
                    float offsetX = (panelW - drawW) * 0.5f;
                    if (offsetX > 0.0f) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + offsetX);
                    ImGui::Image((ImTextureID)m_PerDrawPreviewDescSet, ImVec2(drawW, drawH));
                    ImGui::TextDisabled("HDR linear; clipping above 1.0 is expected.");
                }

                UI::EndCollapsingHeader();
                return;
            }
        }

        // -------------------------------------------------------------------
        // Pass-output archive path (Phase 14D fallback).
        // -------------------------------------------------------------------
        if (m_SelArchiveIdx < 0 || m_SelArchiveIdx >= (int)capture.archivedImages.size())
        {
            ImGui::TextDisabled("(no output preview for this event)");
            UI::EndCollapsingHeader();
            return;
        }

        const auto& archive = capture.archivedImages[m_SelArchiveIdx];

        if (m_SelArchiveLayer >= 0)
            ImGui::Text("%s  (%ux%u, layer %d)", archive.name.c_str(),
                        archive.width, archive.height, m_SelArchiveLayer);
        else
            ImGui::Text("%s  (%ux%u, %u layer%s)",
                        archive.name.c_str(),
                        archive.width, archive.height,
                        archive.layers, archive.layers > 1 ? "s" : "");

        if (archive.isDepth)
        {
            // ---------------------------------------------------------------
            // Phase 14F — Depth archive visualization.
            //
            // For cascade slices (m_SelArchiveLayer >= 0) we use the matching
            // cascade's far view-Z so each slice gets a sensible contrast
            // range instead of clipping against the camera's full draw distance.
            // For non-array depth archives we fall back to a generic 0.1..200 m
            // window matching the existing AddDebugBlitPass default.
            // ---------------------------------------------------------------
            if (sampler == VK_NULL_HANDLE)
            {
                ImGui::TextDisabled("(debug sampler not initialized)");
                UI::EndCollapsingHeader();
                return;
            }

            float nearZ = 0.1f;
            float farZ  = 200.0f;
            if (m_SelArchiveLayer >= 0 && m_SelArchiveLayer < 4)
            {
                // cascadeSplitsViewZ holds absolute view-space distances; pair
                // each slice with [prev_split..this_split] so blacks/whites
                // map to that cascade's actual depth range.
                farZ = capture.cascadeSplitsViewZ[m_SelArchiveLayer];
                if (m_SelArchiveLayer > 0)
                    nearZ = capture.cascadeSplitsViewZ[m_SelArchiveLayer - 1];
                if (farZ <= nearZ) farZ = nearZ + 1.0f;  // sanity fallback
            }

            m_RS->BlitArchivedDepthToPreview((u32)m_SelArchiveIdx, m_SelArchiveLayer, nearZ, farZ);

            VkImageView depthPreview = m_RS->GetDepthPreviewView();
            u32         dpW          = m_RS->GetDepthPreviewWidth();
            u32         dpH          = m_RS->GetDepthPreviewHeight();
            if (depthPreview == VK_NULL_HANDLE || dpW == 0 || dpH == 0)
            {
                ImGui::TextDisabled("(depth preview unavailable)");
                UI::EndCollapsingHeader();
                return;
            }

            if (depthPreview != m_DepthPreviewViewCached)
            {
                if (m_DepthPreviewDescSet != VK_NULL_HANDLE)
                {
                    VkDescriptorSet stale = m_DepthPreviewDescSet;
                    VulkanContext::Get().PushDeletion([stale]() {
                        ImGui_ImplVulkan_RemoveTexture(stale);
                    });
                }
                m_DepthPreviewDescSet = ImGui_ImplVulkan_AddTexture(
                    sampler, depthPreview, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                m_DepthPreviewViewCached = depthPreview;
            }

            if (m_DepthPreviewDescSet != VK_NULL_HANDLE)
            {
                ImGui::TextDisabled("Linearized [%.2f m .. %.2f m]", nearZ, farZ);
                float panelW = ImGui::GetContentRegionAvail().x;
                float maxH   = 320.0f;
                float ar     = (float)dpW / (float)dpH;
                float drawW  = panelW;
                float drawH  = drawW / ar;
                if (drawH > maxH) { drawH = maxH; drawW = drawH * ar; }
                float offsetX = (panelW - drawW) * 0.5f;
                if (offsetX > 0.0f) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + offsetX);
                ImGui::Image((ImTextureID)m_DepthPreviewDescSet, ImVec2(drawW, drawH));
            }

            UI::EndCollapsingHeader();
            return;
        }

        if (sampler == VK_NULL_HANDLE)
        {
            ImGui::TextDisabled("(debug sampler not initialized)");
            UI::EndCollapsingHeader();
            return;
        }

        // Cache by view-pointer identity so recaptures (which create brand-new
        // VkImageViews even when archive indices overlap) trigger a fresh
        // descriptor instead of binding a stale handle to freed GPU memory.
        if (archive.view != m_DisplayArchiveViewCached)
        {
            if (m_DisplayArchiveDescSet != VK_NULL_HANDLE)
            {
                VkDescriptorSet stale = m_DisplayArchiveDescSet;
                VulkanContext::Get().PushDeletion([stale]() {
                    ImGui_ImplVulkan_RemoveTexture(stale);
                });
                m_DisplayArchiveDescSet = VK_NULL_HANDLE;
            }
            m_DisplayArchiveDescSet = ImGui_ImplVulkan_AddTexture(
                sampler, archive.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            m_DisplayArchiveViewCached = archive.view;
        }

        if (m_DisplayArchiveDescSet != VK_NULL_HANDLE)
        {
            float panelW = ImGui::GetContentRegionAvail().x;
            float maxH   = 320.0f;
            float ar     = (float)archive.width / (float)archive.height;
            float drawW  = panelW;
            float drawH  = drawW / ar;
            if (drawH > maxH) { drawH = maxH; drawW = drawH * ar; }
            float offsetX = (panelW - drawW) * 0.5f;
            if (offsetX > 0.0f) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + offsetX);
            ImGui::Image((ImTextureID)m_DisplayArchiveDescSet, ImVec2(drawW, drawH));
        }

        UI::EndCollapsingHeader();
    }

    void FrameDebuggerPanel::DrawSelectedDetailTables(const RG::CapturedFrame& capture)
    {
        // -------- Pass / Cascade selected --------
        if (m_SelKind == RG::EventNodeKind::Pass || m_SelKind == RG::EventNodeKind::Cascade)
        {
            if (m_SelPassIndex < 0 || m_SelPassIndex >= (int)capture.passes.size()) return;
            const auto& pass = capture.passes[m_SelPassIndex];

            if (UI::BeginCollapsingHeader("Pass", true))
            {
                ImGui::Indent(4.0f);
                if (ImGui::BeginTable("##PassInfo", 2)) {
                    ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 130.0f);
                    ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
                    ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextDisabled("Name");        ImGui::TableNextColumn(); ImGui::Text("%s", pass.name.c_str());
                    ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextDisabled("Draw Calls");  ImGui::TableNextColumn(); ImGui::Text("%u", pass.drawCallCount);
                    ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextDisabled("First Draw");  ImGui::TableNextColumn(); ImGui::Text("%u", pass.firstDrawIndex);
                    if (pass.gpuTimeMs >= 0.0f) {
                        ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextDisabled("GPU Time"); ImGui::TableNextColumn(); ImGui::Text("%.3f ms", pass.gpuTimeMs);
                    }
                    if (m_SelKind == RG::EventNodeKind::Cascade) {
                        ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextDisabled("Cascade Layer"); ImGui::TableNextColumn(); ImGui::Text("%d", m_SelArchiveLayer);
                    }
                    ImGui::EndTable();
                }
                ImGui::Unindent(4.0f);
                UI::EndCollapsingHeader();
            }

            // List the archives this pass produced (color/depth/etc.).
            // passArchives is keyed by graph pass index (sparse); m_SelPassIndex
            // is the dense passes[] index, so route through pass.graphPassIndex.
            const u32 archiveKey = pass.graphPassIndex;
            if (archiveKey < capture.passArchives.size() &&
                !capture.passArchives[archiveKey].empty() &&
                UI::BeginCollapsingHeader("Pass Outputs"))
            {
                ImGui::Indent(4.0f);
                for (u32 ai : capture.passArchives[archiveKey])
                {
                    if (ai >= capture.archivedImages.size()) continue;
                    const auto& a = capture.archivedImages[ai];
                    ImGui::BulletText("%s  (%ux%u%s)", a.name.c_str(), a.width, a.height,
                                       a.isDepth ? ", depth" : "");
                }
                ImGui::Unindent(4.0f);
                UI::EndCollapsingHeader();
            }

            // ----------------------------------------------------------------
            // Phase 14F — CSM cascade detail block. Surfaces GPU-true values
            // captured at the time of the snapshot (cascadeSplitsViewZ, biases,
            // per-cascade texel footprint, light-space matrix). All values come
            // from CapturedFrame, NOT live RenderingSystem state, so editing
            // light parameters while frozen doesn't desync the readout.
            // ----------------------------------------------------------------
            if (m_SelKind == RG::EventNodeKind::Cascade &&
                m_SelArchiveLayer >= 0 && m_SelArchiveLayer < 4 &&
                UI::BeginCollapsingHeader("Cascade", true))
            {
                const int  ci    = m_SelArchiveLayer;
                const float prev = (ci > 0) ? capture.cascadeSplitsViewZ[ci - 1] : 0.0f;
                const float cur  = capture.cascadeSplitsViewZ[ci];

                ImGui::Indent(4.0f);
                if (ImGui::BeginTable("##CascadeInfo", 2)) {
                    ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 150.0f);
                    ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
                    ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextDisabled("Cascade Index");      ImGui::TableNextColumn(); ImGui::Text("%d", ci);
                    ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextDisabled("Range (view-Z, m)");  ImGui::TableNextColumn(); ImGui::Text("%.2f .. %.2f", prev, cur);
                    ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextDisabled("Slice Depth (m)");    ImGui::TableNextColumn(); ImGui::Text("%.2f", cur - prev);
                    ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextDisabled("Depth Bias");         ImGui::TableNextColumn(); ImGui::Text("%.6f%s", capture.shadowBias[ci], capture.shadowBias[ci] < 0.0f ? "  (disabled)" : "");
                    ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextDisabled("Normal Bias (texels)"); ImGui::TableNextColumn(); ImGui::Text("%.4f", capture.shadowNormalBias[ci]);
                    ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextDisabled("World Texel Size");   ImGui::TableNextColumn(); ImGui::Text("%.4f m", capture.cascadeTexelSize[ci]);
                    ImGui::EndTable();
                }
                ImGui::Unindent(4.0f);
                UI::EndCollapsingHeader();

                // Optional collapsible: full light-space matrix (16 floats —
                // verbose, hidden by default for casual inspection).
                if (UI::BeginCollapsingHeader("Light-space Matrix"))
                {
                    ImGui::Indent(4.0f);
                    const Mat4& M = capture.lightSpaceMatrix[ci];
                    if (ImGui::BeginTable("##LSM", 4)) {
                        for (int row = 0; row < 4; ++row)
                        {
                            ImGui::TableNextRow();
                            for (int col = 0; col < 4; ++col) {
                                ImGui::TableNextColumn();
                                ImGui::Text("%8.3f", M[col][row]);
                            }
                        }
                        ImGui::EndTable();
                    }
                    ImGui::Unindent(4.0f);
                    UI::EndCollapsingHeader();
                }
            }
            return;
        }

        // -------- Draw call selected --------
        if (m_SelKind != RG::EventNodeKind::Draw) return;
        if (m_SelDrawIndex < 0 || m_SelDrawIndex >= (int)capture.drawCalls.size()) return;
        const auto& dc = capture.drawCalls[m_SelDrawIndex];

        if (UI::BeginCollapsingHeader("Draw Call", true))
        {
            ImGui::Indent(4.0f);
            if (ImGui::BeginTable("##DCInfo", 2)) {
                ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 130.0f);
                ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);

                const char* kindStr =
                    (dc.kind == RG::DispatchKind::Compute)         ? "Compute" :
                    (dc.kind == RG::DispatchKind::IndexedIndirect) ? "Indexed Indirect" :
                                                                      "Direct";

                ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextDisabled("Global Index"); ImGui::TableNextColumn(); ImGui::Text("%u", dc.globalIndex);
                ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextDisabled("Kind");         ImGui::TableNextColumn(); ImGui::Text("%s", kindStr);
                ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextDisabled("Pass");         ImGui::TableNextColumn(); ImGui::Text("%s", dc.passName.c_str());

                if (dc.kind == RG::DispatchKind::Compute)
                {
                    ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextDisabled("Shader");        ImGui::TableNextColumn(); ImGui::Text("%s", dc.meshName.c_str());
                    ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextDisabled("Group Count X"); ImGui::TableNextColumn(); ImGui::Text("%u", dc.groupCountX);
                    ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextDisabled("Group Count Y"); ImGui::TableNextColumn(); ImGui::Text("%u", dc.groupCountY);
                    ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextDisabled("Group Count Z"); ImGui::TableNextColumn(); ImGui::Text("%u", dc.groupCountZ);
                    uint64_t invocations = (uint64_t)dc.groupCountX * dc.groupCountY * dc.groupCountZ;
                    ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextDisabled("Invocations");   ImGui::TableNextColumn(); ImGui::Text("%llu groups", (unsigned long long)invocations);
                }
                else
                {
                    ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextDisabled("Mesh");        ImGui::TableNextColumn(); ImGui::Text("%s", dc.meshName.c_str());
                    ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextDisabled("Entity");      ImGui::TableNextColumn(); ImGui::Text("%s", dc.entityName.c_str());
                    ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextDisabled("Index Count"); ImGui::TableNextColumn(); ImGui::Text("%u", dc.indexCount);
                    if (dc.kind == RG::DispatchKind::IndexedIndirect)
                    {
                        ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextDisabled("GPU Object Index"); ImGui::TableNextColumn(); ImGui::Text("%u", dc.gpuObjectIndex);
                        ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextDisabled("Indirect Offset"); ImGui::TableNextColumn(); ImGui::Text("%llu B", (unsigned long long)dc.indirectOffset);
                        ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextDisabled("Draw Count");      ImGui::TableNextColumn(); ImGui::Text("%u", dc.indirectDrawCount);
                        ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextDisabled("Stride");          ImGui::TableNextColumn(); ImGui::Text("%u B", dc.indirectStride);
                    }
                }
                ImGui::EndTable();
            }
            ImGui::Unindent(4.0f);
            UI::EndCollapsingHeader();
        }

        ImGui::Spacing();

        if (UI::BeginCollapsingHeader("Pipeline State", true))
        {
            ImGui::Indent(4.0f);
            const auto& ps = dc.pipelineState;
            if (ImGui::BeginTable("##PSInfo", 2)) {
                ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 130.0f);
                ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextDisabled("Shader");       ImGui::TableNextColumn(); ImGui::Text("%s", ps.shaderName.c_str());
                ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextDisabled("Render Mode");  ImGui::TableNextColumn(); ImGui::Text("%s", RenderModeToString(ps.renderMode));
                ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextDisabled("Cull Mode");    ImGui::TableNextColumn(); ImGui::Text("%s", CullModeToString(ps.cullMode));
                ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextDisabled("Polygon Mode"); ImGui::TableNextColumn(); ImGui::Text("%s", PolygonModeToString(ps.polygonMode));
                ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextDisabled("Skinned");      ImGui::TableNextColumn(); ImGui::Text("%s", ps.isSkinned ? "Yes" : "No");
                ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextDisabled("Depth Test");   ImGui::TableNextColumn(); ImGui::Text("%s", ps.depthTest ? "On" : "Off");
                ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextDisabled("Depth Write");  ImGui::TableNextColumn(); ImGui::Text("%s", ps.depthWrite ? "On" : "Off");
                ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextDisabled("Blend");        ImGui::TableNextColumn(); ImGui::Text("%s", ps.blendEnabled ? "On" : "Off");
                ImGui::EndTable();
            }
            ImGui::Unindent(4.0f);
            UI::EndCollapsingHeader();
        }

        ImGui::Spacing();

        const bool isGraphicsDraw = (dc.kind != RG::DispatchKind::Compute);
        if (isGraphicsDraw && UI::BeginCollapsingHeader("Transform"))
        {
            ImGui::Indent(4.0f);
            Vec3 scale, translation, skew;
            Vec4 perspective;
            Quat rotation;
            Math::Decompose(dc.modelMatrix, scale, rotation, translation, skew, perspective);
            Vec3 euler = Math::Degrees(Math::EulerAngles(rotation));

            if (ImGui::BeginTable("##TransformInfo", 2)) {
                ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 130.0f);
                ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextDisabled("Position"); ImGui::TableNextColumn();
                ImGui::Text("%.2f, %.2f, %.2f", translation.x, translation.y, translation.z);
                ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextDisabled("Rotation"); ImGui::TableNextColumn();
                ImGui::Text("%.1f, %.1f, %.1f", euler.x, euler.y, euler.z);
                ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextDisabled("Scale");    ImGui::TableNextColumn();
                ImGui::Text("%.2f, %.2f, %.2f", scale.x, scale.y, scale.z);
                ImGui::EndTable();
            }
            ImGui::Unindent(4.0f);
            UI::EndCollapsingHeader();
        }

        ImGui::Spacing();

        // Indirect draws read per-object data from the SSBO, but the panel
        // still surfaces what the FrameDebugger captured at submission time.
        if (isGraphicsDraw && UI::BeginCollapsingHeader("Push Constants"))
        {
            ImGui::Indent(4.0f);
            if (ImGui::BeginTable("##PCInfo", 2)) {
                ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 130.0f);
                ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextDisabled("Material Index"); ImGui::TableNextColumn(); ImGui::Text("%u", dc.materialIndex);
                ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextDisabled("Shade Mode");     ImGui::TableNextColumn(); ImGui::Text("%u", dc.shadeMode);
                ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextDisabled("Entity ID");      ImGui::TableNextColumn(); ImGui::Text("%u", dc.entityID);
                ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextDisabled("Bone Offset");    ImGui::TableNextColumn(); ImGui::Text("%u", dc.boneOffset);
                ImGui::EndTable();
            }
            ImGui::Unindent(4.0f);
            UI::EndCollapsingHeader();
        }
    }
}
