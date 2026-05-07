#pragma once

#include <imgui.h>

#include <filesystem>
#include <optional>
#include <string>

struct ImFont;

namespace Luth
{
    // ImGui style and font preset. Serialized as JSON under luth/assets/styles/ for engine
    // built-ins and as project-local overrides under the user's project. Applied at editor
    // init and again whenever the user picks a different style in EditorSettings.
    struct FontConfig {
        std::string MainFontName;       // e.g. "Roboto-Regular.ttf"
        float       MainFontSize;       // e.g. 15.0f
        bool        MergeMainWithSolid; // true for Custom style (merge FA-Solid into main font)
        float       IconFontSize;       // e.g. 14.0f or 48.0f
    };

    struct StylePreset {
        std::string Name;

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

    struct StyleFile {
        StylePreset Preset;
        FontConfig  Font;
    };

    namespace EditorStyle
    {
        // Load fonts for a given style. Sets Editor font pointers.
        void LoadFonts(const FontConfig& config);

        // Apply a preset's style properties and colors
        void Apply(const StylePreset& preset);

        // JSON (de)serialise. File format pairs a StylePreset with its FontConfig.
        std::optional<StyleFile> LoadFromFile(const std::filesystem::path& path);
        bool SaveToFile(const StylePreset& preset, const FontConfig& font, const std::filesystem::path& path);

        // Load one of the built-in styles shipped under luth/assets/styles/<name>.json.
        std::optional<StyleFile> LoadBuiltin(const std::string& name);
    }
}
