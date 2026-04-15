#include "luthpch.h"
#include "luth/editor/panels/FrameDebuggerPanel.h"
#include "luth/scene/Systems.h"
#include "luth/editor/UI.h"
#include "luth/utils/LuthIcons.h"

#include <vulkan/vulkan.h>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/matrix_decompose.hpp>

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
        m_RS = Systems::GetSystem<RenderingSystem>();
    }

    // ---- Main Render ----

    void FrameDebuggerPanel::OnRender()
    {
        if (!m_RS) return;

        ImGui::PushFont(Editor::GetFASolid());
        std::string title = ICON_FA_DIAGRAM_PROJECT + std::string("  Frame Debugger");
        ImGui::Begin(title.c_str());
        ImGui::PopFont();

        auto debuggerState = m_RS->GetDebuggerState();

        if (debuggerState == DebuggerState::Frozen && m_RS->GetCapturedFrame().valid)
        {
            DrawCaptureView(m_RS->GetCapturedFrame());
        }
        else
        {
            auto& snapshot = m_RS->GetGraphSnapshot();
            DrawLiveView(snapshot);
        }

        ImGui::End();
    }

    // =========================================================================
    //  Live Mode (pass-level, same as before)
    // =========================================================================

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
        // Enable button — triggers capture
        if (ImGui::Button("Enable"))
        {
            m_RS->RequestCapture();
            m_DrawCallSlider    = -1;
            m_SelectedDrawIndex = -1;
        }

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

    // =========================================================================
    //  Capture Mode (draw-call-level stepping)
    // =========================================================================

    void FrameDebuggerPanel::DrawCaptureView(const RG::CapturedFrame& capture)
    {
        DrawCaptureControlBar(capture);
        ImGui::Separator();

        float availWidth = ImGui::GetContentRegionAvail().x;
        float leftWidth = availWidth * 0.35f;
        if (leftWidth < 200.0f) leftWidth = 200.0f;

        ImGui::BeginChild("##CapturePassTree", ImVec2(leftWidth, 0), ImGuiChildFlags_ResizeX | ImGuiChildFlags_Borders);
        DrawCapturePassTree(capture);
        ImGui::EndChild();

        ImGui::SameLine();

        float rightWidth = ImGui::GetContentRegionAvail().x;
        if (rightWidth < 280.0f) rightWidth = 280.0f;
        ImGui::BeginChild("##CaptureDetails", ImVec2(rightWidth, 0), ImGuiChildFlags_Borders);
        DrawCaptureDrawCallDetails(capture);
        ImGui::EndChild();
    }

    void FrameDebuggerPanel::DrawCaptureControlBar(const RG::CapturedFrame& capture)
    {
        // Disable button — exits freeze
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.7f, 0.2f, 0.2f, 1.0f));
        if (ImGui::Button("Disable"))
        {
            m_RS->ExitCapture();
            m_DrawCallSlider    = -1;
            m_SelectedDrawIndex = -1;
        }
        ImGui::PopStyleColor();

        ImGui::SameLine();

        int totalDraws = (int)capture.drawCalls.size();
        if (totalDraws == 0)
        {
            ImGui::TextDisabled("No draw calls captured");
            return;
        }

        // Initialize slider on first frame
        if (m_DrawCallSlider < 0)
            m_DrawCallSlider = totalDraws;

        // Draw call slider
        float arrowsWidth = 60.0f;
        float labelWidth = 120.0f;
        float sliderWidth = ImGui::GetContentRegionAvail().x - arrowsWidth - labelWidth - ImGui::GetStyle().ItemSpacing.x * 3;
        if (sliderWidth < 80.0f) sliderWidth = 80.0f;

        ImGui::SetNextItemWidth(sliderWidth);
        if (ImGui::SliderInt("##DrawSlider", &m_DrawCallSlider, 0, totalDraws))
        {
            m_RS->SetDebuggerDrawLimit((u32)m_DrawCallSlider);
            if (m_DrawCallSlider > 0 && m_DrawCallSlider <= totalDraws)
                m_SelectedDrawIndex = m_DrawCallSlider - 1;
            else
                m_SelectedDrawIndex = -1;
        }

        ImGui::SameLine();

        if (ImGui::ArrowButton("##PrevDraw", ImGuiDir_Left))
        {
            if (m_DrawCallSlider > 0) {
                m_DrawCallSlider--;
                m_RS->SetDebuggerDrawLimit((u32)m_DrawCallSlider);
                m_SelectedDrawIndex = m_DrawCallSlider > 0 ? m_DrawCallSlider - 1 : -1;
            }
        }
        ImGui::SameLine();
        if (ImGui::ArrowButton("##NextDraw", ImGuiDir_Right))
        {
            if (m_DrawCallSlider < totalDraws) {
                m_DrawCallSlider++;
                m_RS->SetDebuggerDrawLimit((u32)m_DrawCallSlider);
                m_SelectedDrawIndex = m_DrawCallSlider > 0 ? m_DrawCallSlider - 1 : -1;
            }
        }

        // Draw count label
        ImGui::SameLine();
        char label[64];
        snprintf(label, sizeof(label), "Draw %d / %d", m_DrawCallSlider, totalDraws);
        float textW = ImGui::CalcTextSize(label).x;
        float rightEdge = ImGui::GetWindowContentRegionMax().x;
        if (rightEdge - ImGui::GetCursorPosX() > textW)
            ImGui::SetCursorPosX(rightEdge - textW);
        ImGui::Text("%s", label);
    }

    void FrameDebuggerPanel::DrawCapturePassTree(const RG::CapturedFrame& capture)
    {
        for (u32 pi = 0; pi < (u32)capture.passes.size(); pi++)
        {
            auto& pass = capture.passes[pi];

            // Pass node — expandable if it has draw calls
            ImGuiTreeNodeFlags passFlags = ImGuiTreeNodeFlags_SpanAvailWidth;
            if (pass.drawCallCount == 0)
                passFlags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
            else
                passFlags |= ImGuiTreeNodeFlags_DefaultOpen;

            // Highlight if selected draw is in this pass
            bool passSelected = false;
            if (m_SelectedDrawIndex >= 0)
            {
                u32 selIdx = (u32)m_SelectedDrawIndex;
                passSelected = (selIdx >= pass.firstDrawIndex && selIdx < pass.firstDrawIndex + pass.drawCallCount);
            }

            char passLabel[128];
            snprintf(passLabel, sizeof(passLabel), "%s  (%u draws)", pass.name.c_str(), pass.drawCallCount);

            bool nodeOpen = ImGui::TreeNodeEx((void*)(intptr_t)(pi + 10000), passFlags, "%s", passLabel);

            // Right-aligned GPU time
            if (pass.gpuTimeMs >= 0.0f)
            {
                char timeBuf[32];
                snprintf(timeBuf, sizeof(timeBuf), "%.2f ms", pass.gpuTimeMs);
                float tw = ImGui::CalcTextSize(timeBuf).x;
                float avail = ImGui::GetWindowContentRegionMax().x;
                ImGui::SameLine(avail - tw);
                ImGui::TextDisabled("%s", timeBuf);
            }

            // Click on pass → jump slider to last draw of this pass
            if (ImGui::IsItemClicked(ImGuiMouseButton_Left))
            {
                u32 passEnd = pass.firstDrawIndex + pass.drawCallCount;
                m_DrawCallSlider    = (int)passEnd;
                m_SelectedDrawIndex = passEnd > 0 ? (int)(passEnd - 1) : -1;
                m_RS->SetDebuggerDrawLimit((u32)m_DrawCallSlider);
            }

            if (nodeOpen && pass.drawCallCount > 0)
            {
                for (u32 di = 0; di < pass.drawCallCount; di++)
                {
                    u32 globalIdx = pass.firstDrawIndex + di;
                    if (globalIdx >= (u32)capture.drawCalls.size()) break;
                    auto& dc = capture.drawCalls[globalIdx];

                    bool isSelected = ((int)globalIdx == m_SelectedDrawIndex);
                    ImGuiTreeNodeFlags dcFlags = ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_SpanAvailWidth;
                    if (isSelected) dcFlags |= ImGuiTreeNodeFlags_Selected;

                    const char* kindPrefix =
                        (dc.kind == RG::DispatchKind::Compute)         ? "[C] " :
                        (dc.kind == RG::DispatchKind::IndexedIndirect) ? "[I] " : "";
                    char dcLabel[128];
                    snprintf(dcLabel, sizeof(dcLabel), "%sDraw %u: %s (%s)", kindPrefix, globalIdx, dc.meshName.c_str(), dc.pipelineState.shaderName.c_str());
                    ImGui::TreeNodeEx((void*)(intptr_t)(globalIdx + 20000), dcFlags, "%s", dcLabel);

                    // Right-aligned index count
                    if (dc.indexCount > 0)
                    {
                        char idxBuf[32];
                        snprintf(idxBuf, sizeof(idxBuf), "%u idx", dc.indexCount);
                        float tw = ImGui::CalcTextSize(idxBuf).x;
                        float avail = ImGui::GetWindowContentRegionMax().x;
                        ImGui::SameLine(avail - tw);
                        ImGui::TextDisabled("%s", idxBuf);
                    }

                    if (ImGui::IsItemClicked(ImGuiMouseButton_Left))
                    {
                        m_SelectedDrawIndex = (int)globalIdx;
                        m_DrawCallSlider    = (int)(globalIdx + 1);
                        m_RS->SetDebuggerDrawLimit((u32)m_DrawCallSlider);
                    }
                }
                ImGui::TreePop();
            }
        }
    }

    void FrameDebuggerPanel::DrawCaptureDrawCallDetails(const RG::CapturedFrame& capture)
    {
        if (m_SelectedDrawIndex < 0 || m_SelectedDrawIndex >= (int)capture.drawCalls.size())
        {
            ImGui::TextDisabled("Select a draw call to view details");
            return;
        }

        auto& dc = capture.drawCalls[m_SelectedDrawIndex];

        // ---- Output Preview ----
        if (UI::BeginCollapsingHeader("Output", true))
        {
            auto tex = m_RS->GetSceneColor();
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
            UI::EndCollapsingHeader();
        }

        ImGui::Spacing();

        // ---- Draw Call Info ----
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
                ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextDisabled("Kind"); ImGui::TableNextColumn(); ImGui::Text("%s", kindStr);
                ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextDisabled("Pass"); ImGui::TableNextColumn(); ImGui::Text("%s", dc.passName.c_str());

                if (dc.kind == RG::DispatchKind::Compute)
                {
                    ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextDisabled("Shader"); ImGui::TableNextColumn(); ImGui::Text("%s", dc.meshName.c_str());
                    ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextDisabled("Group Count X"); ImGui::TableNextColumn(); ImGui::Text("%u", dc.groupCountX);
                    ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextDisabled("Group Count Y"); ImGui::TableNextColumn(); ImGui::Text("%u", dc.groupCountY);
                    ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextDisabled("Group Count Z"); ImGui::TableNextColumn(); ImGui::Text("%u", dc.groupCountZ);
                    uint64_t invocations = (uint64_t)dc.groupCountX * dc.groupCountY * dc.groupCountZ;
                    ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextDisabled("Invocations"); ImGui::TableNextColumn(); ImGui::Text("%llu groups", (unsigned long long)invocations);
                }
                else
                {
                    ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextDisabled("Mesh"); ImGui::TableNextColumn(); ImGui::Text("%s", dc.meshName.c_str());
                    ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextDisabled("Entity"); ImGui::TableNextColumn(); ImGui::Text("%s", dc.entityName.c_str());
                    ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextDisabled("Index Count"); ImGui::TableNextColumn(); ImGui::Text("%u", dc.indexCount);

                    if (dc.kind == RG::DispatchKind::IndexedIndirect)
                    {
                        ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextDisabled("GPU Object Index"); ImGui::TableNextColumn(); ImGui::Text("%u", dc.gpuObjectIndex);
                        ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextDisabled("Indirect Offset"); ImGui::TableNextColumn(); ImGui::Text("%llu B", (unsigned long long)dc.indirectOffset);
                        ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextDisabled("Draw Count"); ImGui::TableNextColumn(); ImGui::Text("%u", dc.indirectDrawCount);
                        ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextDisabled("Stride"); ImGui::TableNextColumn(); ImGui::Text("%u B", dc.indirectStride);
                    }
                }

                ImGui::EndTable();
            }
            ImGui::Unindent(4.0f);
            UI::EndCollapsingHeader();
        }

        ImGui::Spacing();

        // ---- Pipeline State ----
        if (UI::BeginCollapsingHeader("Pipeline State", true))
        {
            ImGui::Indent(4.0f);
            auto& ps = dc.pipelineState;
            if (ImGui::BeginTable("##PSInfo", 2)) {
                ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 130.0f);
                ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);

                ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextDisabled("Shader"); ImGui::TableNextColumn(); ImGui::Text("%s", ps.shaderName.c_str());
                ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextDisabled("Render Mode"); ImGui::TableNextColumn(); ImGui::Text("%s", RenderModeToString(ps.renderMode));
                ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextDisabled("Cull Mode"); ImGui::TableNextColumn(); ImGui::Text("%s", CullModeToString(ps.cullMode));
                ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextDisabled("Polygon Mode"); ImGui::TableNextColumn(); ImGui::Text("%s", PolygonModeToString(ps.polygonMode));
                ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextDisabled("Skinned"); ImGui::TableNextColumn(); ImGui::Text("%s", ps.isSkinned ? "Yes" : "No");
                ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextDisabled("Depth Test"); ImGui::TableNextColumn(); ImGui::Text("%s", ps.depthTest ? "On" : "Off");
                ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextDisabled("Depth Write"); ImGui::TableNextColumn(); ImGui::Text("%s", ps.depthWrite ? "On" : "Off");
                ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextDisabled("Blend"); ImGui::TableNextColumn(); ImGui::Text("%s", ps.blendEnabled ? "On" : "Off");

                ImGui::EndTable();
            }
            ImGui::Unindent(4.0f);
            UI::EndCollapsingHeader();
        }

        ImGui::Spacing();

        // Transform / push constants are only meaningful for graphics draws.
        bool isGraphicsDraw = (dc.kind != RG::DispatchKind::Compute);

        // ---- Transform (decompose model matrix) ----
        if (isGraphicsDraw && UI::BeginCollapsingHeader("Transform"))
        {
            ImGui::Indent(4.0f);

            glm::vec3 scale, translation, skew;
            glm::vec4 perspective;
            glm::quat rotation;
            glm::decompose(dc.modelMatrix, scale, rotation, translation, skew, perspective);

            glm::vec3 euler = glm::degrees(glm::eulerAngles(rotation));

            if (ImGui::BeginTable("##TransformInfo", 2)) {
                ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 130.0f);
                ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);

                ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextDisabled("Position"); ImGui::TableNextColumn();
                ImGui::Text("%.2f, %.2f, %.2f", translation.x, translation.y, translation.z);

                ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextDisabled("Rotation"); ImGui::TableNextColumn();
                ImGui::Text("%.1f, %.1f, %.1f", euler.x, euler.y, euler.z);

                ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextDisabled("Scale"); ImGui::TableNextColumn();
                ImGui::Text("%.2f, %.2f, %.2f", scale.x, scale.y, scale.z);

                ImGui::EndTable();
            }
            ImGui::Unindent(4.0f);
            UI::EndCollapsingHeader();
        }

        ImGui::Spacing();

        // ---- Push Constants ----
        // Note: after Phase 12D/F, graphics draws are indirect and read per-object data from the SSBO,
        // not push constants. These fields are still populated for indirect draws via the SSBO record.
        if (isGraphicsDraw && UI::BeginCollapsingHeader("Push Constants"))
        {
            ImGui::Indent(4.0f);
            if (ImGui::BeginTable("##PCInfo", 2)) {
                ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 130.0f);
                ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);

                ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextDisabled("Material Index"); ImGui::TableNextColumn(); ImGui::Text("%u", dc.materialIndex);
                ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextDisabled("Shade Mode"); ImGui::TableNextColumn(); ImGui::Text("%u", dc.shadeMode);
                ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextDisabled("Entity ID"); ImGui::TableNextColumn(); ImGui::Text("%u", dc.entityID);
                ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextDisabled("Bone Offset"); ImGui::TableNextColumn(); ImGui::Text("%u", dc.boneOffset);

                ImGui::EndTable();
            }
            ImGui::Unindent(4.0f);
            UI::EndCollapsingHeader();
        }
    }
}
