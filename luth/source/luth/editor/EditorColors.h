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

        // Selection outline (used by RenderingSystem outline pass)
        static inline ImVec4 SelectionOutline = { 1.0f, 0.6f, 0.0f, 1.0f }; // Orange

        // Status colors
        static inline ImVec4 ErrorRed      = { 0.9f, 0.2f, 0.2f, 1.0f };
        static inline ImVec4 WarningYellow = { 0.9f, 0.8f, 0.2f, 1.0f };
        static inline ImVec4 SuccessGreen  = { 0.2f, 0.9f, 0.4f, 1.0f };
        static inline ImVec4 InfoBlue      = { 0.4f, 0.6f, 0.9f, 1.0f };

        // Worker thread status (profiler panel)
        static inline ImU32 WorkerRunning  = IM_COL32(100, 200,  50, 255); // Green
        static inline ImU32 WorkerIdle     = IM_COL32(100, 100, 100, 255); // Grey
        static inline ImU32 WorkerStealing = IM_COL32(220, 190,  40, 255); // Yellow
        static inline ImU32 WorkerSleeping = IM_COL32( 60,  60,  80, 255); // Dim blue-grey

        // Memory category colors (profiler stacked bar)
        static inline ImU32 MemGeneral     = IM_COL32( 80, 140, 220, 255); // Blue
        static inline ImU32 MemRendering   = IM_COL32(100, 200,  80, 255); // Green
        static inline ImU32 MemScene       = IM_COL32( 80, 200, 200, 255); // Cyan
        static inline ImU32 MemJobs        = IM_COL32(220, 190,  40, 255); // Yellow
        static inline ImU32 MemResources   = IM_COL32(160, 100, 220, 255); // Purple
        static inline ImU32 MemEditor      = IM_COL32(220, 140,  40, 255); // Orange
        static inline ImU32 MemFrameLinear = IM_COL32( 60, 180, 160, 255); // Teal
        static inline ImU32 MemFrameTagged = IM_COL32(200, 120, 160, 255); // Pink
        static inline ImU32 MemGPU         = IM_COL32( 90,  90, 200, 255); // Deep Indigo
    };
}
