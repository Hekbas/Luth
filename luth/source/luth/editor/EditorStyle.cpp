#include "luthpch.h"
#include "luth/editor/EditorStyle.h"
#include "luth/editor/Editor.h"
#include "luth/resources/FileSystem.h"
#include "luth/utils/LuthIcons.h"

namespace Luth::EditorStyle
{
    // ================================================================
    // Font Loading
    // ================================================================

    static ImFont* LoadIconFont(const char* filename, float size, bool mergeMode)
    {
        std::string path = FileSystem::GetPath(AssetType::Font, filename).string();
        if (!fs::exists(path)) {
            LH_CORE_WARN("Icon font not found: {}", path);
            return nullptr;
        }

        ImFontConfig config;
        config.MergeMode = mergeMode;
        config.PixelSnapH = mergeMode;
        static const ImWchar iconRanges[] = { ICON_MIN_FA, ICON_MAX_FA, 0 };

        ImFont* font = ImGui::GetIO().Fonts->AddFontFromFileTTF(path.c_str(), size, &config, iconRanges);
        if (!font) {
            LH_CORE_WARN("Failed to load icon font: {}", path);
        }
        return font;
    }

    void LoadFonts(const FontConfig& config)
    {
        ImGuiIO& io = ImGui::GetIO();

        // Main font
        std::string mainPath = FileSystem::GetPath(AssetType::Font, config.MainFontName).string();
        if (fs::exists(mainPath)) {
            ImFontConfig fontCfg;
            fontCfg.OversampleH = 3;
            fontCfg.OversampleV = 2;
            fontCfg.PixelSnapH  = true;
            Editor::MainFontRef() = io.Fonts->AddFontFromFileTTF(mainPath.c_str(), config.MainFontSize, &fontCfg);
        }
        else {
            LH_CORE_WARN("Main font not found: {}", mainPath);
        }

        if (config.MergeMainWithSolid) {
            // Custom style: merge FA-Solid into main font, then add default + standalone FA-Regular
            Editor::FASolidRef() = LoadIconFont("fa-solid-900.ttf", config.IconFontSize, true);
            io.Fonts->AddFontDefault();
            Editor::FARegularRef() = LoadIconFont("fa-regular-400.ttf", config.IconFontSize, false);
        }
        else {
            // Bubblegum/Matrix: add default after main, standalone icon fonts
            io.Fonts->AddFontDefault();
            Editor::FARegularRef() = LoadIconFont("fa-regular-400.ttf", config.IconFontSize, false);
            Editor::FASolidRef() = LoadIconFont("fa-solid-900.ttf", config.IconFontSize, false);
        }
    }

    // ================================================================
    // Style Application
    // ================================================================

    void Apply(const StylePreset& preset)
    {
        ImGuiStyle& style = ImGui::GetStyle();

        style.WindowRounding    = preset.WindowRounding;
        style.ChildRounding     = preset.ChildRounding;
        style.FrameRounding     = preset.FrameRounding;
        style.GrabRounding      = preset.GrabRounding;
        style.PopupRounding     = preset.PopupRounding;
        style.ScrollbarRounding = preset.ScrollbarRounding;
        style.TabRounding       = preset.TabRounding;

        style.WindowBorderSize  = preset.WindowBorderSize;
        style.ChildBorderSize   = preset.ChildBorderSize;
        style.PopupBorderSize   = preset.PopupBorderSize;
        style.FrameBorderSize   = preset.FrameBorderSize;
        style.TabBorderSize     = preset.TabBorderSize;

        style.WindowPadding     = preset.WindowPadding;
        style.FramePadding      = preset.FramePadding;
        style.ItemSpacing       = preset.ItemSpacing;
        style.ItemInnerSpacing  = preset.ItemInnerSpacing;
        style.TouchExtraPadding = preset.TouchExtraPadding;
        style.IndentSpacing     = preset.IndentSpacing;
        style.ScrollbarSize     = preset.ScrollbarSize;
        style.GrabMinSize       = preset.GrabMinSize;
        style.Alpha             = preset.Alpha;

        style.WindowMenuButtonPosition = ImGuiDir_None;

        memcpy(style.Colors, preset.Colors, sizeof(preset.Colors));
    }

    // ================================================================
    // Preset Definitions
    // ================================================================

    const StylePreset& Custom()
    {
        static StylePreset p = []() {
            StylePreset s{};
            s.Name = "Custom";

            s.WindowRounding = 5.0f; s.ChildRounding = 5.0f; s.FrameRounding = 3.0f;
            s.GrabRounding = 3.0f; s.PopupRounding = 5.0f; s.ScrollbarRounding = 2.0f; s.TabRounding = 3.0f;

            s.WindowBorderSize = 0.0f; s.ChildBorderSize = 0.0f; s.PopupBorderSize = 0.0f;
            s.FrameBorderSize = 0.0f; s.TabBorderSize = 0.0f;

            s.WindowPadding = {8,8}; s.FramePadding = {6,4}; s.ItemSpacing = {6,4};
            s.ItemInnerSpacing = {4,4}; s.TouchExtraPadding = {0,0};
            s.IndentSpacing = 20; s.ScrollbarSize = 14; s.GrabMinSize = 12;
            s.Alpha = 0.95f;

            auto& c = s.Colors;
            c[ImGuiCol_Text]                   = {0.90f, 0.90f, 0.90f, 1.00f};
            c[ImGuiCol_TextDisabled]           = {0.60f, 0.60f, 0.60f, 1.00f};
            c[ImGuiCol_WindowBg]               = {0.12f, 0.12f, 0.12f, 0.94f};
            c[ImGuiCol_ChildBg]                = {0.15f, 0.15f, 0.15f, 0.90f};
            c[ImGuiCol_PopupBg]                = {0.11f, 0.11f, 0.11f, 0.94f};
            c[ImGuiCol_Border]                 = {0.08f, 0.08f, 0.08f, 0.80f};
            c[ImGuiCol_BorderShadow]           = {0.00f, 0.00f, 0.00f, 0.00f};
            c[ImGuiCol_FrameBg]                = {0.20f, 0.20f, 0.20f, 0.80f};
            c[ImGuiCol_FrameBgHovered]         = {0.25f, 0.25f, 0.25f, 0.80f};
            c[ImGuiCol_FrameBgActive]          = {0.30f, 0.30f, 0.30f, 0.80f};
            c[ImGuiCol_Button]                 = {0.20f, 0.20f, 0.20f, 0.80f};
            c[ImGuiCol_ButtonHovered]          = {0.30f, 0.30f, 0.30f, 0.80f};
            c[ImGuiCol_ButtonActive]           = {0.35f, 0.35f, 0.35f, 0.80f};
            c[ImGuiCol_Header]                 = {0.25f, 0.25f, 0.25f, 0.80f};
            c[ImGuiCol_HeaderHovered]          = {0.30f, 0.30f, 0.30f, 0.80f};
            c[ImGuiCol_HeaderActive]           = {0.35f, 0.35f, 0.35f, 0.80f};
            c[ImGuiCol_SliderGrab]             = {0.70f, 0.70f, 0.70f, 0.60f};
            c[ImGuiCol_SliderGrabActive]       = {0.85f, 0.85f, 0.85f, 0.60f};
            c[ImGuiCol_ScrollbarBg]            = {0.10f, 0.10f, 0.10f, 0.60f};
            c[ImGuiCol_ScrollbarGrab]          = {0.40f, 0.40f, 0.40f, 0.60f};
            c[ImGuiCol_ScrollbarGrabHovered]   = {0.50f, 0.50f, 0.50f, 0.60f};
            c[ImGuiCol_ScrollbarGrabActive]    = {0.60f, 0.60f, 0.60f, 0.60f};
            c[ImGuiCol_CheckMark]              = {0.90f, 0.90f, 0.90f, 0.90f};
            c[ImGuiCol_Separator]              = {0.30f, 0.30f, 0.30f, 0.60f};
            c[ImGuiCol_SeparatorHovered]       = {0.40f, 0.40f, 0.40f, 0.78f};
            c[ImGuiCol_SeparatorActive]        = {0.50f, 0.50f, 0.50f, 1.00f};
            c[ImGuiCol_ResizeGrip]             = {0.40f, 0.40f, 0.40f, 0.60f};
            c[ImGuiCol_ResizeGripHovered]      = {0.60f, 0.60f, 0.60f, 0.60f};
            c[ImGuiCol_ResizeGripActive]       = {0.80f, 0.80f, 0.80f, 0.60f};
            c[ImGuiCol_Tab]                    = {0.15f, 0.15f, 0.15f, 0.86f};
            c[ImGuiCol_TabHovered]             = {0.25f, 0.25f, 0.25f, 0.86f};
            c[ImGuiCol_TabActive]              = {0.20f, 0.20f, 0.20f, 0.86f};
            c[ImGuiCol_TabSelectedOverline]    = {0.35f, 0.35f, 0.35f, 1.00f};
            c[ImGuiCol_TabUnfocused]           = {0.15f, 0.15f, 0.15f, 0.86f};
            c[ImGuiCol_TabUnfocusedActive]     = {0.18f, 0.18f, 0.18f, 0.86f};
            c[ImGuiCol_TitleBg]                = {0.10f, 0.10f, 0.10f, 0.85f};
            c[ImGuiCol_TitleBgActive]          = {0.12f, 0.12f, 0.12f, 0.90f};
            c[ImGuiCol_TitleBgCollapsed]       = {0.10f, 0.10f, 0.10f, 0.60f};
            c[ImGuiCol_DockingPreview]         = {0.90f, 0.90f, 0.90f, 0.70f};
            c[ImGuiCol_DockingEmptyBg]         = {0.00f, 0.00f, 0.00f, 0.00f};
            c[ImGuiCol_MenuBarBg]              = {0.15f, 0.15f, 0.15f, 0.90f};
            c[ImGuiCol_TableHeaderBg]          = {0.19f, 0.19f, 0.20f, 1.00f};
            c[ImGuiCol_TableBorderStrong]      = {0.31f, 0.31f, 0.35f, 1.00f};
            c[ImGuiCol_TableBorderLight]       = {0.23f, 0.23f, 0.25f, 1.00f};
            c[ImGuiCol_PlotLines]              = {0.90f, 0.90f, 0.90f, 1.00f};
            c[ImGuiCol_PlotLinesHovered]       = {1.00f, 0.43f, 0.35f, 1.00f};
            c[ImGuiCol_PlotHistogram]          = {0.90f, 0.70f, 0.00f, 1.00f};
            c[ImGuiCol_PlotHistogramHovered]   = {1.00f, 0.60f, 0.00f, 1.00f};
            c[ImGuiCol_TextSelectedBg]         = {0.25f, 0.50f, 0.75f, 0.50f};
            c[ImGuiCol_DragDropTarget]         = {0.70f, 0.70f, 0.70f, 0.90f};
            c[ImGuiCol_NavHighlight]           = {0.45f, 0.45f, 0.90f, 0.80f};
            c[ImGuiCol_NavWindowingHighlight]  = {1.00f, 1.00f, 1.00f, 0.70f};
            c[ImGuiCol_NavWindowingDimBg]      = {0.80f, 0.80f, 0.80f, 0.20f};
            c[ImGuiCol_ModalWindowDimBg]       = {0.20f, 0.20f, 0.20f, 0.35f};
            return s;
        }();
        return p;
    }

    const StylePreset& Bubblegum()
    {
        static StylePreset p = []() {
            StylePreset s{};
            s.Name = "Bubblegum";

            s.WindowRounding = 12.0f; s.ChildRounding = 0.0f; s.FrameRounding = 12.0f;
            s.GrabRounding = 12.0f; s.PopupRounding = 8.0f; s.ScrollbarRounding = 12.0f; s.TabRounding = 8.0f;

            s.WindowBorderSize = 0.0f; s.ChildBorderSize = 0.0f; s.PopupBorderSize = 0.0f;
            s.FrameBorderSize = 0.0f; s.TabBorderSize = 0.0f;

            s.WindowPadding = {12,12}; s.FramePadding = {12,6}; s.ItemSpacing = {10,8};
            s.ItemInnerSpacing = {4,4}; s.TouchExtraPadding = {0,0};
            s.IndentSpacing = 20; s.ScrollbarSize = 16; s.GrabMinSize = 12;
            s.Alpha = 1.0f;

            auto& c = s.Colors;
            c[ImGuiCol_Text]                   = {0.25f, 0.16f, 0.29f, 1.00f};
            c[ImGuiCol_TextDisabled]           = {0.65f, 0.55f, 0.65f, 1.00f};
            c[ImGuiCol_WindowBg]               = {1.00f, 0.89f, 0.93f, 1.00f};
            c[ImGuiCol_ChildBg]                = {0.98f, 0.95f, 0.97f, 1.00f};
            c[ImGuiCol_PopupBg]                = {1.00f, 0.95f, 0.98f, 1.00f};
            c[ImGuiCol_Border]                 = {1.00f, 0.70f, 0.82f, 0.50f};
            c[ImGuiCol_BorderShadow]           = {0.00f, 0.00f, 0.00f, 0.00f};
            c[ImGuiCol_FrameBg]                = {1.00f, 0.96f, 0.98f, 1.00f};
            c[ImGuiCol_FrameBgHovered]         = {1.00f, 0.89f, 0.93f, 1.00f};
            c[ImGuiCol_FrameBgActive]          = {1.00f, 0.82f, 0.89f, 1.00f};
            c[ImGuiCol_TitleBg]                = {1.00f, 0.70f, 0.82f, 1.00f};
            c[ImGuiCol_TitleBgActive]          = {1.00f, 0.60f, 0.75f, 1.00f};
            c[ImGuiCol_TitleBgCollapsed]       = {1.00f, 0.89f, 0.93f, 1.00f};
            c[ImGuiCol_MenuBarBg]              = {1.00f, 0.89f, 0.93f, 1.00f};
            c[ImGuiCol_ScrollbarBg]            = {1.00f, 0.95f, 0.97f, 1.00f};
            c[ImGuiCol_ScrollbarGrab]          = {1.00f, 0.70f, 0.82f, 0.50f};
            c[ImGuiCol_ScrollbarGrabHovered]   = {1.00f, 0.60f, 0.75f, 0.50f};
            c[ImGuiCol_ScrollbarGrabActive]    = {1.00f, 0.50f, 0.65f, 0.50f};
            c[ImGuiCol_Button]                 = {1.00f, 0.70f, 0.82f, 1.00f};
            c[ImGuiCol_ButtonHovered]          = {1.00f, 0.80f, 0.89f, 1.00f};
            c[ImGuiCol_ButtonActive]           = {1.00f, 0.60f, 0.75f, 1.00f};
            c[ImGuiCol_Header]                 = {1.00f, 0.70f, 0.82f, 1.00f};
            c[ImGuiCol_HeaderHovered]          = {1.00f, 0.80f, 0.89f, 1.00f};
            c[ImGuiCol_HeaderActive]           = {1.00f, 0.60f, 0.75f, 1.00f};
            c[ImGuiCol_SliderGrab]             = {1.00f, 0.70f, 0.82f, 1.00f};
            c[ImGuiCol_SliderGrabActive]       = {1.00f, 0.60f, 0.75f, 1.00f};
            c[ImGuiCol_CheckMark]              = {0.47f, 0.87f, 0.63f, 1.00f};
            c[ImGuiCol_Separator]              = {1.00f, 0.70f, 0.82f, 0.50f};
            c[ImGuiCol_SeparatorHovered]       = {1.00f, 0.60f, 0.75f, 0.50f};
            c[ImGuiCol_SeparatorActive]        = {1.00f, 0.50f, 0.65f, 0.50f};
            c[ImGuiCol_ResizeGrip]             = {1.00f, 0.70f, 0.82f, 0.20f};
            c[ImGuiCol_ResizeGripHovered]      = {1.00f, 0.60f, 0.75f, 0.67f};
            c[ImGuiCol_ResizeGripActive]       = {1.00f, 0.50f, 0.65f, 0.95f};
            c[ImGuiCol_Tab]                    = {1.00f, 0.85f, 0.92f, 1.00f};
            c[ImGuiCol_TabHovered]             = {1.00f, 0.92f, 0.96f, 1.00f};
            c[ImGuiCol_TabActive]              = {1.00f, 0.70f, 0.82f, 1.00f};
            c[ImGuiCol_TabUnfocused]           = {1.00f, 0.90f, 0.94f, 1.00f};
            c[ImGuiCol_TabUnfocusedActive]     = {1.00f, 0.80f, 0.88f, 1.00f};
            c[ImGuiCol_TabDimmed]              = {0.95f, 0.82f, 0.89f, 1.00f};
            c[ImGuiCol_TabDimmedSelected]      = {1.00f, 0.75f, 0.85f, 1.00f};
            c[ImGuiCol_DockingPreview]         = {1.00f, 0.70f, 0.82f, 0.70f};
            c[ImGuiCol_DockingEmptyBg]         = {1.00f, 0.95f, 0.97f, 1.00f};
            c[ImGuiCol_PlotLines]              = {1.00f, 0.70f, 0.82f, 1.00f};
            c[ImGuiCol_PlotLinesHovered]       = {1.00f, 0.60f, 0.75f, 1.00f};
            c[ImGuiCol_PlotHistogram]          = {0.47f, 0.87f, 0.63f, 1.00f};
            c[ImGuiCol_PlotHistogramHovered]   = {0.40f, 0.80f, 0.55f, 1.00f};
            c[ImGuiCol_TableHeaderBg]          = {1.00f, 0.89f, 0.93f, 1.00f};
            c[ImGuiCol_TableBorderStrong]      = {1.00f, 0.70f, 0.82f, 1.00f};
            c[ImGuiCol_TableBorderLight]       = {1.00f, 0.80f, 0.89f, 1.00f};
            c[ImGuiCol_TextSelectedBg]         = {1.00f, 0.70f, 0.82f, 0.35f};
            c[ImGuiCol_DragDropTarget]         = {0.47f, 0.87f, 0.63f, 1.00f};
            c[ImGuiCol_NavHighlight]           = {0.47f, 0.87f, 0.63f, 0.80f};
            c[ImGuiCol_NavWindowingHighlight]  = {1.00f, 0.70f, 0.82f, 0.70f};
            c[ImGuiCol_NavWindowingDimBg]      = {0.80f, 0.80f, 0.80f, 0.20f};
            c[ImGuiCol_ModalWindowDimBg]       = {0.80f, 0.80f, 0.80f, 0.35f};
            return s;
        }();
        return p;
    }

    const StylePreset& Matrix()
    {
        static StylePreset p = []() {
            StylePreset s{};
            s.Name = "Matrix";

            s.WindowRounding = 4.0f; s.ChildRounding = 0.0f; s.FrameRounding = 2.0f;
            s.GrabRounding = 2.0f; s.PopupRounding = 0.0f; s.ScrollbarRounding = 4.0f; s.TabRounding = 0.0f;

            s.WindowBorderSize = 0.0f; s.ChildBorderSize = 0.0f; s.PopupBorderSize = 0.0f;
            s.FrameBorderSize = 0.0f; s.TabBorderSize = 0.0f;

            s.WindowPadding = {8,8}; s.FramePadding = {6,4}; s.ItemSpacing = {6,4};
            s.ItemInnerSpacing = {4,4}; s.TouchExtraPadding = {0,0};
            s.IndentSpacing = 20; s.ScrollbarSize = 14; s.GrabMinSize = 12;
            s.Alpha = 1.0f;

            auto& c = s.Colors;
            c[ImGuiCol_Text]                   = {0.00f, 1.00f, 0.00f, 1.00f};
            c[ImGuiCol_TextDisabled]           = {0.00f, 0.40f, 0.00f, 1.00f};
            c[ImGuiCol_WindowBg]               = {0.00f, 0.02f, 0.00f, 1.00f};
            c[ImGuiCol_ChildBg]                = {0.00f, 0.02f, 0.00f, 0.00f};
            c[ImGuiCol_PopupBg]                = {0.00f, 0.03f, 0.00f, 0.94f};
            c[ImGuiCol_Border]                 = {0.00f, 0.50f, 0.00f, 0.50f};
            c[ImGuiCol_BorderShadow]           = {0.00f, 0.00f, 0.00f, 0.00f};
            c[ImGuiCol_FrameBg]                = {0.00f, 0.05f, 0.00f, 0.54f};
            c[ImGuiCol_FrameBgHovered]         = {0.00f, 0.30f, 0.00f, 0.40f};
            c[ImGuiCol_FrameBgActive]          = {0.00f, 0.40f, 0.00f, 0.67f};
            c[ImGuiCol_Button]                 = {0.00f, 0.20f, 0.00f, 0.40f};
            c[ImGuiCol_ButtonHovered]          = {0.00f, 0.50f, 0.00f, 1.00f};
            c[ImGuiCol_ButtonActive]           = {0.00f, 0.70f, 0.00f, 1.00f};
            c[ImGuiCol_Header]                 = {0.00f, 0.30f, 0.00f, 0.31f};
            c[ImGuiCol_HeaderHovered]          = {0.00f, 0.50f, 0.00f, 0.80f};
            c[ImGuiCol_HeaderActive]           = {0.00f, 0.70f, 0.00f, 1.00f};
            c[ImGuiCol_SliderGrab]             = {0.00f, 0.60f, 0.00f, 1.00f};
            c[ImGuiCol_SliderGrabActive]       = {0.00f, 0.80f, 0.00f, 1.00f};
            c[ImGuiCol_ScrollbarGrab]          = {0.00f, 0.30f, 0.00f, 0.80f};
            c[ImGuiCol_ScrollbarGrabHovered]   = {0.00f, 0.50f, 0.00f, 0.80f};
            c[ImGuiCol_ScrollbarGrabActive]    = {0.00f, 0.70f, 0.00f, 1.00f};
            c[ImGuiCol_CheckMark]              = {0.00f, 1.00f, 0.00f, 1.00f};
            c[ImGuiCol_Separator]              = {0.00f, 0.50f, 0.00f, 0.50f};
            c[ImGuiCol_SeparatorHovered]       = {0.00f, 0.70f, 0.00f, 0.60f};
            c[ImGuiCol_SeparatorActive]        = {0.00f, 0.90f, 0.00f, 0.70f};
            c[ImGuiCol_ResizeGrip]             = {0.00f, 0.30f, 0.00f, 0.20f};
            c[ImGuiCol_ResizeGripHovered]      = {0.00f, 0.60f, 0.00f, 0.60f};
            c[ImGuiCol_ResizeGripActive]       = {0.00f, 0.80f, 0.00f, 0.90f};
            c[ImGuiCol_Tab]                    = {0.00f, 0.15f, 0.00f, 0.86f};
            c[ImGuiCol_TabHovered]             = {0.00f, 0.50f, 0.00f, 0.80f};
            c[ImGuiCol_TabSelected]            = {0.00f, 0.30f, 0.00f, 1.00f};
            c[ImGuiCol_TabSelectedOverline]    = {0.00f, 0.50f, 0.00f, 1.00f};
            c[ImGuiCol_TabUnfocused]           = {0.00f, 0.10f, 0.00f, 0.97f};
            c[ImGuiCol_TabUnfocusedActive]     = {0.00f, 0.20f, 0.00f, 1.00f};
            c[ImGuiCol_TitleBg]                = {0.00f, 0.10f, 0.00f, 1.00f};
            c[ImGuiCol_TitleBgActive]          = {0.00f, 0.20f, 0.00f, 1.00f};
            c[ImGuiCol_TitleBgCollapsed]       = {0.00f, 0.10f, 0.00f, 0.51f};
            c[ImGuiCol_DockingPreview]         = {0.00f, 0.60f, 0.00f, 0.70f};
            c[ImGuiCol_DockingEmptyBg]         = {0.00f, 0.05f, 0.00f, 1.00f};
            return s;
        }();
        return p;
    }

    const StylePreset& Rider()
    {
        static StylePreset p = []() {
            StylePreset s{};
            s.Name = "Rider";

            // Rider is very flat — minimal rounding, compact spacing
            s.WindowRounding = 2.0f; s.ChildRounding = 0.0f; s.FrameRounding = 2.0f;
            s.GrabRounding = 2.0f; s.PopupRounding = 2.0f; s.ScrollbarRounding = 2.0f; s.TabRounding = 0.0f;

            s.WindowBorderSize = 1.0f; s.ChildBorderSize = 1.0f; s.PopupBorderSize = 1.0f;
            s.FrameBorderSize = 0.0f; s.TabBorderSize = 0.0f;

            s.WindowPadding = {6,6}; s.FramePadding = {4,3}; s.ItemSpacing = {6,3};
            s.ItemInnerSpacing = {4,4}; s.TouchExtraPadding = {0,0};
            s.IndentSpacing = 16; s.ScrollbarSize = 12; s.GrabMinSize = 10;
            s.Alpha = 1.0f;

            // JetBrains Rider Dark palette
            // Bg:      #1E1F22 (deepest), #2B2D30 (panel), #313335 (subtle raised)
            // Frame:   #1E1F22 (input fields — same as deepest bg)
            // Text:    #BCBEC4 (primary), #6F737A (disabled)
            // Accent:  #4A88C7 (blue) — ONLY on selected tabs + overlines
            // Border:  #393B40 (subtle), #43454A (stronger)
            // Hover:   #2E3038 (very subtle brightening, NO blue)
            // Select:  #2E436E (dark blue tint for selected items)

            auto& c = s.Colors;
            c[ImGuiCol_Text]                   = {0.74f, 0.75f, 0.77f, 1.00f}; // #BCBEC4
            c[ImGuiCol_TextDisabled]           = {0.44f, 0.45f, 0.48f, 1.00f}; // #6F737A
            c[ImGuiCol_WindowBg]               = {0.17f, 0.18f, 0.19f, 1.00f}; // #2B2D30
            c[ImGuiCol_ChildBg]                = {0.12f, 0.12f, 0.13f, 1.00f}; // #1E1F22
            c[ImGuiCol_PopupBg]                = {0.17f, 0.18f, 0.19f, 0.98f}; // #2B2D30
            c[ImGuiCol_Border]                 = {0.22f, 0.23f, 0.25f, 1.00f}; // #393B40
            c[ImGuiCol_BorderShadow]           = {0.00f, 0.00f, 0.00f, 0.00f};
            c[ImGuiCol_FrameBg]                = {0.12f, 0.12f, 0.13f, 1.00f}; // #1E1F22 (dark, like Rider inputs)
            c[ImGuiCol_FrameBgHovered]         = {0.15f, 0.15f, 0.17f, 1.00f}; // very subtle brighten
            c[ImGuiCol_FrameBgActive]          = {0.18f, 0.19f, 0.21f, 1.00f}; // slightly brighter, no blue
            c[ImGuiCol_TitleBg]                = {0.12f, 0.12f, 0.13f, 1.00f}; // #1E1F22
            c[ImGuiCol_TitleBgActive]          = {0.12f, 0.12f, 0.13f, 1.00f}; // same — flat
            c[ImGuiCol_TitleBgCollapsed]       = {0.12f, 0.12f, 0.13f, 0.75f};
            c[ImGuiCol_MenuBarBg]              = {0.12f, 0.12f, 0.13f, 1.00f}; // #1E1F22
            c[ImGuiCol_ScrollbarBg]            = {0.12f, 0.12f, 0.13f, 0.00f}; // transparent
            c[ImGuiCol_ScrollbarGrab]          = {0.30f, 0.31f, 0.33f, 0.60f}; // subtle
            c[ImGuiCol_ScrollbarGrabHovered]   = {0.36f, 0.37f, 0.39f, 0.80f};
            c[ImGuiCol_ScrollbarGrabActive]    = {0.42f, 0.43f, 0.45f, 0.80f};
            c[ImGuiCol_CheckMark]              = {0.29f, 0.53f, 0.78f, 1.00f}; // #4A88C7
            c[ImGuiCol_SliderGrab]             = {0.29f, 0.53f, 0.78f, 1.00f}; // #4A88C7
            c[ImGuiCol_SliderGrabActive]       = {0.36f, 0.60f, 0.85f, 1.00f};
            c[ImGuiCol_Button]                 = {0.17f, 0.18f, 0.19f, 1.00f}; // flat, matches window bg
            c[ImGuiCol_ButtonHovered]          = {0.22f, 0.23f, 0.25f, 1.00f}; // subtle gray brighten, NO blue
            c[ImGuiCol_ButtonActive]           = {0.26f, 0.27f, 0.29f, 1.00f}; // gray press
            c[ImGuiCol_Header]                 = {0.17f, 0.18f, 0.19f, 1.00f}; // flat — same as window bg
            c[ImGuiCol_HeaderHovered]          = {0.22f, 0.23f, 0.25f, 1.00f}; // subtle gray, NO blue
            c[ImGuiCol_HeaderActive]           = {0.18f, 0.26f, 0.43f, 1.00f}; // #2E436E dark blue tint (selected)
            c[ImGuiCol_Separator]              = {0.22f, 0.23f, 0.25f, 1.00f}; // #393B40
            c[ImGuiCol_SeparatorHovered]       = {0.30f, 0.31f, 0.33f, 1.00f}; // gray
            c[ImGuiCol_SeparatorActive]        = {0.36f, 0.37f, 0.39f, 1.00f}; // gray
            c[ImGuiCol_ResizeGrip]             = {0.22f, 0.23f, 0.25f, 0.20f};
            c[ImGuiCol_ResizeGripHovered]      = {0.30f, 0.31f, 0.33f, 0.60f};
            c[ImGuiCol_ResizeGripActive]       = {0.36f, 0.37f, 0.39f, 0.90f};
            c[ImGuiCol_Tab]                    = {0.12f, 0.12f, 0.13f, 1.00f}; // #1E1F22 inactive
            c[ImGuiCol_TabHovered]             = {0.20f, 0.21f, 0.22f, 1.00f}; // subtle gray hover
            c[ImGuiCol_TabActive]              = {0.17f, 0.18f, 0.19f, 1.00f}; // #2B2D30 matches window
            c[ImGuiCol_TabSelectedOverline]    = {0.29f, 0.53f, 0.78f, 1.00f}; // #4A88C7 — THE blue accent
            c[ImGuiCol_TabUnfocused]           = {0.12f, 0.12f, 0.13f, 1.00f};
            c[ImGuiCol_TabUnfocusedActive]     = {0.15f, 0.15f, 0.16f, 1.00f};
            c[ImGuiCol_DockingPreview]         = {0.29f, 0.53f, 0.78f, 0.40f};
            c[ImGuiCol_DockingEmptyBg]         = {0.12f, 0.12f, 0.13f, 1.00f};
            c[ImGuiCol_PlotLines]              = {0.53f, 0.54f, 0.57f, 1.00f}; // gray lines
            c[ImGuiCol_PlotLinesHovered]       = {0.29f, 0.53f, 0.78f, 1.00f};
            c[ImGuiCol_PlotHistogram]          = {0.29f, 0.53f, 0.78f, 0.80f}; // blue bars
            c[ImGuiCol_PlotHistogramHovered]   = {0.36f, 0.60f, 0.85f, 1.00f};
            c[ImGuiCol_TableHeaderBg]          = {0.14f, 0.14f, 0.15f, 1.00f};
            c[ImGuiCol_TableBorderStrong]      = {0.22f, 0.23f, 0.25f, 1.00f};
            c[ImGuiCol_TableBorderLight]       = {0.18f, 0.19f, 0.20f, 1.00f};
            c[ImGuiCol_TextSelectedBg]         = {0.18f, 0.26f, 0.43f, 0.60f}; // #2E436E
            c[ImGuiCol_DragDropTarget]         = {0.29f, 0.53f, 0.78f, 0.90f};
            c[ImGuiCol_NavHighlight]           = {0.29f, 0.53f, 0.78f, 0.80f};
            c[ImGuiCol_NavWindowingHighlight]  = {1.00f, 1.00f, 1.00f, 0.70f};
            c[ImGuiCol_NavWindowingDimBg]      = {0.80f, 0.80f, 0.80f, 0.20f};
            c[ImGuiCol_ModalWindowDimBg]       = {0.00f, 0.00f, 0.00f, 0.50f};
            return s;
        }();
        return p;
    }
}
