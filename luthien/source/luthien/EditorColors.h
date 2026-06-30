#pragma once

#include <imgui.h>

namespace Luth
{
    // Centralized semantic editor colors. All panels reference these constants instead of
    // hardcoding RGB literals — a single source of truth for axis tints, status colors, gizmo
    // overlays, profiler-panel category colors, and so on.
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

        // Gizmo overlay colors
        static inline ImU32 GizmoCamera       = IM_COL32(180, 180, 220, 180); // Light blue-grey
        static inline ImU32 GizmoAABB         = IM_COL32(160, 160, 160, 100); // Soft grey
        static inline ImU32 GizmoAABBSelected = IM_COL32(255, 160,   0, 200); // Orange for selected
        static inline ImU32 GizmoBoneLine     = IM_COL32(  0, 255, 128, 200); // Green skeleton lines
        static inline ImU32 GizmoBoneJoint    = IM_COL32(255, 255,   0, 255); // Yellow joints
        static inline ImU32 GizmoFog          = IM_COL32(184, 140, 242, 140); // Purple — matches EntityFX hierarchy tint
        static inline ImU32 GizmoWind         = IM_COL32(184, 140, 242, 204); // Purple — matches EntityFX hierarchy tint

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

        // Hierarchy entity-icon category tints (glyph only; entity names stay neutral)
        static inline ImVec4 EntityMesh    = { 0.55f, 0.68f, 0.88f, 1.0f }; // calm blue
        static inline ImVec4 EntityCamera  = { 0.78f, 0.80f, 0.84f, 1.0f }; // silver-grey
        static inline ImVec4 EntityLight   = { 0.95f, 0.80f, 0.25f, 1.0f }; // yellow
        static inline ImVec4 EntityBone    = { 0.30f, 0.85f, 0.45f, 1.0f }; // green (matches GizmoBoneLine)
        static inline ImVec4 EntityAnim    = { 0.25f, 0.78f, 0.75f, 1.0f }; // teal
        static inline ImVec4 EntityPhysics = { 0.95f, 0.55f, 0.20f, 1.0f }; // orange
        static inline ImVec4 EntityFX      = { 0.72f, 0.55f, 0.95f, 1.0f }; // purple
    };
}
