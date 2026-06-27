#pragma once

#include "luth/core/types/LuthTypes.h"
#include <imgui.h>

// Lightweight data-viz widgets for the Profiler (and any panel that needs them): section headers,
// metric cards, labeled bars, stacked bars, and inline legends. Keeps panels off raw ImGui drawing.

namespace Luth::UI
{
    // Faux-bold text (double-draw; the editor ships no bold font). printf-style.
    void BoldText(const char* fmt, ...);

    // Bold section header with breathing room above. Leaves the line open — SameLine after it to add
    // right-aligned content (a legend or numbers); otherwise the next widget flows below.
    void SectionHeader(const char* label);

    // Section header — label followed by a rule filling the row. The editor's SeparatorText.
    void SeparatorText(const char* label);

    // Thin horizontal rule.
    void Separator();

    // Filled area graph (transparent fill + line). Taller, softer alternative to ImGui::PlotLines.
    void AreaGraph(const char* id, const float* values, int count, float maxVal, ImU32 color, float height = 48.0f);

    // Metric card: muted label over a large value on a subtle filled surface. Lay several across a row
    // with SameLine; pass an explicit width (split GetContentRegionAvail by the card count yourself).
    void MetricCard(const char* label, const char* value, float width, float height = 0.0f);

    // Labeled horizontal bar: optional left label (reserve labelWidth to align a column of them), a
    // track + fill (frac 0..1), an optional right-aligned value, and an optional budget tick (tickFrac<0 = none).
    // width<=0 fills the row (minus the value). valueWidth>0 fixes the value column so the bar doesn't
    // jump when the value gains/loses a digit — pass it together with an explicit width for stable bars.
    void StatBar(const char* label, float frac, ImU32 color, const char* value,
                 float labelWidth = 0.0f, float tickFrac = -1.0f, float width = -1.0f, float valueWidth = -1.0f);

    struct BarSegment { float frac; ImU32 color; };
    // Horizontal stacked bar (segment fractions sum to ~1 of the track). Optional peak tick (peakFrac<0 = none).
    // width<=0 fills the content region; pass an explicit width to leave room for trailing text on the row.
    void StackedBar(const char* id, const BarSegment* segments, int count, float height = 14.0f,
                    float peakFrac = -1.0f, float width = -1.0f);

    // Inline legend chip: colored swatch + label (+ optional trailing value). Advance with SameLine.
    void LegendItem(const char* label, ImU32 color, const char* value = nullptr);
}
