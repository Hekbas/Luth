#include "luthpch.h"
#include "luth/editor/panels/FrameDebuggerPanel.h"
#include "luth/scene/Systems.h"
#include "luth/editor/UI.h"
#include "luth/utils/LuthIcons.h"

#include <vulkan/vulkan.h>

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

    // Map a real pass index to its non-culled slider position
    static int PassIndexToSlider(const RG::RenderGraphSnapshot& snapshot, int passIdx)
    {
        int counter = 0;
        for (int i = 0; i < (int)snapshot.passes.size(); i++)
        {
            if (snapshot.passes[i].culled) continue;
            if (i == passIdx) return counter;
            counter++;
        }
        return 0;
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

        auto& snapshot = m_RS->GetGraphSnapshot();
        if (snapshot.passes.empty())
        {
            ImGui::TextDisabled("No render graph data");
            ImGui::End();
            return;
        }

        // Count non-culled passes
        int nonCulledCount = 0;
        for (auto& p : snapshot.passes)
            if (!p.culled) nonCulledCount++;

        // Ensure defaults are valid
        if (nonCulledCount > 0)
        {
            if (m_EventSliderValue < 0) m_EventSliderValue = 0;
            if (m_SelectedPassIndex < 0) m_SelectedPassIndex = SliderToPassIndex(snapshot, 0);
        }

        // ── Top Control Bar ──
        DrawControlBar(snapshot, nonCulledCount);

        ImGui::Separator();

        // ── Split Layout ──
        float availWidth = ImGui::GetContentRegionAvail().x;
        float leftWidth = availWidth * 0.35f;
        if (leftWidth < 180.0f) leftWidth = 180.0f;

        // Left panel — pass tree
        ImGui::BeginChild("##PassTree", ImVec2(leftWidth, 0), ImGuiChildFlags_ResizeX | ImGuiChildFlags_Borders);
        DrawPassTree(snapshot);
        ImGui::EndChild();

        ImGui::SameLine();

        // Right panel — details + preview (min width 250)
        float rightWidth = ImGui::GetContentRegionAvail().x;
        if (rightWidth < 250.0f) rightWidth = 250.0f;
        ImGui::BeginChild("##PassDetails", ImVec2(rightWidth, 0), ImGuiChildFlags_Borders);
        DrawPassDetails(snapshot);
        ImGui::EndChild();

        ImGui::End();
    }

    // ── Control Bar ──

    void FrameDebuggerPanel::DrawControlBar(const RG::RenderGraphSnapshot& snapshot, int nonCulledCount)
    {
        // Enable/Disable toggle button
        if (m_Enabled)
        {
            ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
            if (ImGui::Button("Enabled"))
                m_Enabled = false;
            ImGui::PopStyleColor();
        }
        else
        {
            if (ImGui::Button("Disabled"))
                m_Enabled = true;
        }

        ImGui::SameLine();

        // Event slider — takes most of the space
        float arrowsWidth = 60.0f;
        float timeWidth = 80.0f;
        float sliderWidth = ImGui::GetContentRegionAvail().x - arrowsWidth - timeWidth - ImGui::GetStyle().ItemSpacing.x * 3;
        if (sliderWidth < 80.0f) sliderWidth = 80.0f;

        ImGui::SetNextItemWidth(sliderWidth);
        if (ImGui::SliderInt("##EventSlider", &m_EventSliderValue, 0, nonCulledCount - 1))
        {
            m_SelectedPassIndex = SliderToPassIndex(snapshot, m_EventSliderValue);
            // Reset resource override so output tracks the new pass
            auto& pass = snapshot.passes[m_SelectedPassIndex];
            m_SelectedResourceIndex = pass.primaryOutputIndex;
        }

        ImGui::SameLine();

        // Previous / Next arrow buttons (to the right of slider)
        if (ImGui::ArrowButton("##PrevPass", ImGuiDir_Left))
        {
            if (m_EventSliderValue > 0)
            {
                m_EventSliderValue--;
                m_SelectedPassIndex = SliderToPassIndex(snapshot, m_EventSliderValue);
                m_SelectedResourceIndex = snapshot.passes[m_SelectedPassIndex].primaryOutputIndex;
            }
        }
        ImGui::SameLine();
        if (ImGui::ArrowButton("##NextPass", ImGuiDir_Right))
        {
            if (m_EventSliderValue < nonCulledCount - 1)
            {
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

    // ── Left Panel: Pass Tree ──

    void FrameDebuggerPanel::DrawPassTree(const RG::RenderGraphSnapshot& snapshot)
    {
        int nonCulledIdx = 0;
        for (int i = 0; i < (int)snapshot.passes.size(); i++)
        {
            auto& pass = snapshot.passes[i];
            if (pass.culled) continue;

            bool isSelected = (m_SelectedPassIndex == i);

            ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_Leaf
                                     | ImGuiTreeNodeFlags_NoTreePushOnOpen
                                     | ImGuiTreeNodeFlags_SpanAvailWidth;
            if (isSelected)
                flags |= ImGuiTreeNodeFlags_Selected;

            ImGui::TreeNodeEx((void*)(intptr_t)i, flags, "%s", pass.name.c_str());

            // Handle click on the tree item
            if (ImGui::IsItemClicked(ImGuiMouseButton_Left))
            {
                m_SelectedPassIndex = i;
                m_EventSliderValue = nonCulledIdx;
                // Reset resource override to track pass output
                m_SelectedResourceIndex = pass.primaryOutputIndex;
            }

            // Right-aligned timing + draw count
            {
                char infoBuf[64];
                if (pass.gpuTimeMs >= 0.0f)
                {
                    if (pass.drawCalls > 0)
                        snprintf(infoBuf, sizeof(infoBuf), "%u  %.2f ms", pass.drawCalls, pass.gpuTimeMs);
                    else
                        snprintf(infoBuf, sizeof(infoBuf), "%.2f ms", pass.gpuTimeMs);
                }
                else
                {
                    snprintf(infoBuf, sizeof(infoBuf), "--");
                }

                float textWidth = ImGui::CalcTextSize(infoBuf).x;
                float availX = ImGui::GetWindowContentRegionMax().x;
                ImGui::SameLine(availX - textWidth);
                ImGui::TextDisabled("%s", infoBuf);
            }

            nonCulledIdx++;
        }
    }

    // ── Right Panel: Pass Details ──

    void FrameDebuggerPanel::DrawPassDetails(const RG::RenderGraphSnapshot& snapshot)
    {
        if (m_SelectedPassIndex < 0 || m_SelectedPassIndex >= (int)snapshot.passes.size())
        {
            ImGui::TextDisabled("Select a render pass to view details");
            return;
        }

        auto& pass = snapshot.passes[m_SelectedPassIndex];

        // ── Output Preview ──
        if (ImGui::CollapsingHeader("Output", ImGuiTreeNodeFlags_DefaultOpen))
        {
            int previewIdx = m_SelectedResourceIndex;
            if (previewIdx < 0) previewIdx = pass.primaryOutputIndex;

            if (previewIdx >= 0 && previewIdx < (int)snapshot.resources.size())
            {
                auto& res = snapshot.resources[previewIdx];
                ImGui::Text("%s  (%ux%u %s)", res.name.c_str(), res.width, res.height, FormatToString(res.format));

                // Depth textures have no sampler — cannot be previewed via ImGui
                if (IsDepthFormat(res.format))
                {
                    ImGui::TextDisabled("(depth buffer - no preview)");
                }
                else
                {
                    auto tex = m_RS->GetNamedTexture(res.name);
                    if (tex)
                    {
                        // Constrained preview: keep aspect ratio, max height 300px
                        float panelW = ImGui::GetContentRegionAvail().x;
                        float maxH = 300.0f;
                        float texW = (float)tex->GetWidth();
                        float texH = (float)tex->GetHeight();
                        float ar = texW / texH;

                        float drawW = panelW;
                        float drawH = drawW / ar;
                        if (drawH > maxH)
                        {
                            drawH = maxH;
                            drawW = drawH * ar;
                        }

                        // Center horizontally
                        float offsetX = (panelW - drawW) * 0.5f;
                        if (offsetX > 0.0f)
                            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + offsetX);

                        ImTextureID texID = UI::GetTextureID(tex);
                        // Render targets are in Vulkan's native top-down layout — use standard UVs
                        ImGui::Image(texID, ImVec2(drawW, drawH), ImVec2(0, 0), ImVec2(1, 1));
                    }
                    else
                    {
                        ImGui::TextDisabled("(no preview available)");
                    }
                }
            }
            else
            {
                ImGui::TextDisabled("No output resource");
            }
        }

        ImGui::Spacing();

        // ── Details ──
        if (ImGui::CollapsingHeader("Details", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::Indent(4.0f);

            // --- Render Target group ---
            if (pass.primaryOutputIndex >= 0 && pass.primaryOutputIndex < (int)snapshot.resources.size())
            {
                auto& res = snapshot.resources[pass.primaryOutputIndex];

                if (ImGui::BeginTable("##RTInfo", 2, ImGuiTableFlags_None))
                {
                    ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 130.0f);
                    ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);

                    ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextDisabled("Render Target"); ImGui::TableNextColumn(); ImGui::Text("%s", res.name.c_str());
                    ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextDisabled("Size");          ImGui::TableNextColumn(); ImGui::Text("%ux%u", res.width, res.height);
                    ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextDisabled("Format");        ImGui::TableNextColumn(); ImGui::Text("%s", FormatToString(res.format));

                    ImGui::EndTable();
                }
            }

            ImGui::Spacing();
            ImGui::Spacing();

            // --- Pipeline State group ---
            if (ImGui::BeginTable("##PipelineState", 2, ImGuiTableFlags_None))
            {
                ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 130.0f);
                ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);

                ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextDisabled("Depth Test");  ImGui::TableNextColumn(); ImGui::Text("%s", pass.depthTest ? "On" : "Off");
                ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextDisabled("Depth Write"); ImGui::TableNextColumn(); ImGui::Text("%s", pass.depthWrite ? "On" : "Off");
                ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextDisabled("Blend");       ImGui::TableNextColumn(); ImGui::Text("%s", pass.blendEnabled ? "On" : "Off");
                ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextDisabled("Cull Mode");   ImGui::TableNextColumn(); ImGui::Text("%s", CullModeToString(pass.cullMode));

                ImGui::EndTable();
            }

            ImGui::Spacing();
            ImGui::Spacing();

            // --- Geometry Stats group ---
            if (ImGui::BeginTable("##GeoStats", 2, ImGuiTableFlags_None))
            {
                ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 130.0f);
                ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);

                ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextDisabled("Draw Calls"); ImGui::TableNextColumn(); ImGui::Text("%u", pass.drawCalls);
                if (pass.indices > 0)
                {
                    ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextDisabled("Indices"); ImGui::TableNextColumn(); ImGui::Text("%u", pass.indices);
                }

                ImGui::EndTable();
            }

            ImGui::Spacing();
            ImGui::Spacing();

            // --- Shader group ---
            if (ImGui::BeginTable("##ShaderInfo", 2, ImGuiTableFlags_None))
            {
                ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 130.0f);
                ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);

                if (!pass.shaderName.empty())
                {
                    ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextDisabled("Shader"); ImGui::TableNextColumn(); ImGui::Text("%s", pass.shaderName.c_str());
                }
                if (pass.gpuTimeMs >= 0.0f)
                {
                    ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextDisabled("GPU Time"); ImGui::TableNextColumn(); ImGui::Text("%.3f ms", pass.gpuTimeMs);
                }

                ImGui::EndTable();
            }

            ImGui::Unindent(4.0f);
        }

        ImGui::Spacing();

        // ── Resources ──
        if (ImGui::CollapsingHeader("Resources"))
        {
            ImGui::Indent(4.0f);

            if (!pass.reads.empty())
            {
                ImGui::TextDisabled("Reads:");
                ImGui::SameLine();
                for (size_t r = 0; r < pass.reads.size(); r++)
                {
                    if (r > 0) ImGui::SameLine();
                    if (ImGui::SmallButton(pass.reads[r].name.c_str()))
                        m_SelectedResourceIndex = (int)pass.reads[r].index - 1;
                }
            }

            if (!pass.writes.empty())
            {
                ImGui::TextDisabled("Writes:");
                ImGui::SameLine();
                for (size_t w = 0; w < pass.writes.size(); w++)
                {
                    if (w > 0) ImGui::SameLine();
                    if (ImGui::SmallButton(pass.writes[w].name.c_str()))
                        m_SelectedResourceIndex = (int)pass.writes[w].index - 1;
                }
            }

            ImGui::Unindent(4.0f);
        }
    }
}
