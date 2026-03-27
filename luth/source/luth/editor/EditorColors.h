#pragma once

#include <imgui.h>

namespace Luth
{
    /// Centralized semantic editor colors.
    /// All panels should reference these instead of hardcoding RGB values.
    struct EditorColors
    {
        // Axis colors (XYZ/W buttons in vector widgets)
        static inline ImVec4 AxisX = { 0.8f, 0.1f, 0.15f, 1.0f }; // Red
        static inline ImVec4 AxisY = { 0.2f, 0.7f, 0.2f,  1.0f }; // Green
        static inline ImVec4 AxisZ = { 0.1f, 0.25f, 0.8f, 1.0f }; // Blue
        static inline ImVec4 AxisW = { 0.3f, 0.3f, 0.3f,  1.0f }; // Grey

        // Tree hierarchy lines
        static inline ImColor TreeLine        = ImColor(80, 80, 80, 128);
        static inline ImColor TreeLineProject = ImColor(128, 128, 128, 128);

        // Drag-and-drop highlight (hierarchy reparenting)
        static inline ImU32 DragHighlight = IM_COL32(0, 255, 255, 255); // Cyan
    };
}
