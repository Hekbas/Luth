#pragma once

#include <imgui.h>

struct ImFont;

namespace Luth
{
    struct FontConfig {
        const char* MainFontName;       // e.g. "Roboto-Regular.ttf"
        float       MainFontSize;       // e.g. 15.0f
        bool        MergeMainWithSolid; // true for Custom style (merge FA-Solid into main font)
        float       IconFontSize;       // e.g. 14.0f or 48.0f
    };

    struct StylePreset {
        const char* Name;
        
        // Docking
        float DockingSeparatorSize;
        
        // Rounding
        float WindowRounding, ChildRounding, FrameRounding, GrabRounding;
        float PopupRounding, ScrollbarRounding, TabRounding;

        // Border
        float WindowBorderSize, ChildBorderSize, PopupBorderSize, FrameBorderSize, TabBorderSize;

        // Padding & spacing
        ImVec2 WindowPadding, FramePadding, ItemSpacing, ItemInnerSpacing, TouchExtraPadding;
        float  IndentSpacing, ScrollbarSize, GrabMinSize;
        float  Alpha;

        // Colors (only the ones that vary between presets)
        ImVec4 Colors[ImGuiCol_COUNT];
    };

    namespace EditorStyle
    {
        // Load fonts for a given style. Sets Editor font pointers.
        void LoadFonts(const FontConfig& config);

        // Apply a preset's style properties and colors
        void Apply(const StylePreset& preset);

        // Built-in presets
        const StylePreset& Custom();
        const StylePreset& Bubblegum();
        const StylePreset& Matrix();
        const StylePreset& Rider();
    }
}
