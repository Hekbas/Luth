#include "lepch.h"
#include "luthien/widgets/Charts.h"

#include <imgui.h>
#include <algorithm>
#include <cstdarg>
#include <cstdio>

namespace Luth::UI
{
    void BoldText(const char* fmt, ...)
    {
        char buf[256];
        va_list args; va_start(args, fmt);
        vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);

        const ImVec2 p   = ImGui::GetCursorScreenPos();
        const ImU32  col = ImGui::GetColorU32(ImGuiCol_Text);
        ImDrawList* dl   = ImGui::GetWindowDrawList();
        dl->AddText(p, col, buf);
        dl->AddText(ImVec2(p.x + 0.6f, p.y), col, buf);   // faux weight
        ImGui::Dummy(ImGui::CalcTextSize(buf));
    }

    void SectionHeader(const char* label)
    {
        ImGui::Dummy(ImVec2(0.0f, 7.0f));   // spacing above the section
        BoldText("%s", label);
    }

    void AreaGraph(const char* id, const float* values, int count, float maxVal, ImU32 color, float height)
    {
        (void)id;
        const float  w = ImGui::GetContentRegionAvail().x;
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(p, ImVec2(p.x + w, p.y + height), ImGui::GetColorU32(ImGuiCol_FrameBg), 3.0f);

        if (count > 1 && maxVal > 0.0f)
        {
            const ImU32 fill = (color & 0x00FFFFFFu) | (50u << 24);   // same hue, ~20% alpha
            const float base = p.y + height;
            for (int i = 0; i + 1 < count; ++i)
            {
                const float x0 = p.x + w * (float)i / (count - 1);
                const float x1 = p.x + w * (float)(i + 1) / (count - 1);
                const float y0 = p.y + height * (1.0f - std::clamp(values[i]     / maxVal, 0.0f, 1.0f));
                const float y1 = p.y + height * (1.0f - std::clamp(values[i + 1] / maxVal, 0.0f, 1.0f));
                dl->AddQuadFilled(ImVec2(x0, y0), ImVec2(x1, y1), ImVec2(x1, base), ImVec2(x0, base), fill);
                dl->AddLine(ImVec2(x0, y0), ImVec2(x1, y1), color, 1.5f);
            }
        }
        ImGui::Dummy(ImVec2(w, height));
    }

    void SeparatorText(const char* label)
    {
        ImGui::TextUnformatted(label);
        ImGui::SameLine();
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float  w = ImGui::GetContentRegionAvail().x;
        const float  y = p.y + ImGui::GetTextLineHeight() * 0.5f;
        if (w > 6.0f)
            ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x + 4, y), ImVec2(p.x + w, y),
                                                ImGui::GetColorU32(ImGuiCol_Separator));
        ImGui::NewLine();
    }

    void Separator()
    {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float  w = ImGui::GetContentRegionAvail().x;
        ImGui::GetWindowDrawList()->AddLine(p, ImVec2(p.x + w, p.y), ImGui::GetColorU32(ImGuiCol_Separator));
        ImGui::Dummy(ImVec2(w, 1.0f));
    }

    void MetricCard(const char* label, const char* value, float width, float height)
    {
        const float lh = ImGui::GetTextLineHeight();
        if (width  <= 0.0f) width  = ImGui::GetContentRegionAvail().x;
        if (height <= 0.0f) height = lh * 2.4f + 16.0f;

        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(p, ImVec2(p.x + width, p.y + height),
                          ImGui::GetColorU32(ImGuiCol_FrameBg), 4.0f);
        dl->AddText(ImVec2(p.x + 10, p.y + 9), ImGui::GetColorU32(ImGuiCol_TextDisabled), label);
        // Larger value glyphs via the draw-list font-size overload (no separate font asset needed).
        dl->AddText(ImGui::GetFont(), lh * 1.45f, ImVec2(p.x + 10, p.y + 9 + lh + 2),
                    ImGui::GetColorU32(ImGuiCol_Text), value);
        ImGui::Dummy(ImVec2(width, height));
    }

    void StatBar(const char* label, float frac, ImU32 color, const char* value,
                 float labelWidth, float tickFrac, float width, float valueWidth)
    {
        frac = std::clamp(frac, 0.0f, 1.0f);
        const float h = 14.0f;

        if (label && *label)
        {
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(label);
            ImGui::SameLine(labelWidth > 0.0f ? labelWidth : 0.0f);
        }

        const float valW = (value && *value)
            ? (valueWidth > 0.0f ? valueWidth : ImGui::CalcTextSize(value).x + 8.0f) : 0.0f;
        float w = width > 0.0f ? width : ImGui::GetContentRegionAvail().x - valW;
        if (w < 8.0f) w = 8.0f;

        const ImVec2 c = ImGui::GetCursorScreenPos();
        const float  y = c.y + (ImGui::GetFrameHeight() - h) * 0.5f;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(ImVec2(c.x, y), ImVec2(c.x + w, y + h), ImGui::GetColorU32(ImGuiCol_FrameBg), 3.0f);
        dl->AddRectFilled(ImVec2(c.x, y), ImVec2(c.x + w * frac, y + h), color, 3.0f);
        if (tickFrac >= 0.0f)
        {
            const float tx = c.x + w * std::clamp(tickFrac, 0.0f, 1.0f);
            dl->AddLine(ImVec2(tx, y), ImVec2(tx, y + h), IM_COL32(255, 255, 255, 150));
        }
        ImGui::Dummy(ImVec2(w, ImGui::GetFrameHeight()));

        if (value && *value)
        {
            ImGui::SameLine();
            const float tw = ImGui::CalcTextSize(value).x;
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, valW - tw));
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(value);
        }
    }

    void StackedBar(const char* id, const BarSegment* segments, int count, float height, float peakFrac, float width)
    {
        ImGui::PushID(id);
        const float w  = width > 0.0f ? width : ImGui::GetContentRegionAvail().x;
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();

        dl->AddRectFilled(p, ImVec2(p.x + w, p.y + height), ImGui::GetColorU32(ImGuiCol_FrameBg), 3.0f);
        float x = 0.0f;
        for (int i = 0; i < count; ++i)
        {
            const float segW = std::clamp(segments[i].frac, 0.0f, 1.0f) * w;
            if (segW > 0.0f)
                dl->AddRectFilled(ImVec2(p.x + x, p.y), ImVec2(p.x + x + segW, p.y + height), segments[i].color);
            x += segW;
        }
        if (peakFrac >= 0.0f)
        {
            const float px = p.x + w * std::clamp(peakFrac, 0.0f, 1.0f);
            dl->AddLine(ImVec2(px, p.y), ImVec2(px, p.y + height), ImGui::GetColorU32(ImGuiCol_Text));
        }
        ImGui::Dummy(ImVec2(w, height));
        ImGui::PopID();
    }

    void LegendItem(const char* label, ImU32 color, const char* value)
    {
        const float sz = ImGui::GetTextLineHeight() * 0.78f;
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float  y = p.y + (ImGui::GetTextLineHeight() - sz) * 0.5f;
        ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(p.x, y), ImVec2(p.x + sz, y + sz), color, 2.0f);
        ImGui::Dummy(ImVec2(sz + 5.0f, ImGui::GetTextLineHeight()));
        ImGui::SameLine(0.0f, 0.0f);
        if (value && *value) ImGui::Text("%s %s", label, value);
        else                 ImGui::TextUnformatted(label);
    }
}
