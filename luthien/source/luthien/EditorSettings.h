#pragma once

#include "luth/core/types/LuthMath.h"

#include <string>
#include <filesystem>
#include <unordered_map>

namespace Luth
{
    struct EditorSettings
    {
        // Style
        std::string activeStyle = "Rider";      // Built-in name (Custom/Bubblegum/Matrix/Rider)
        std::string activeStylePath = "";       // Optional: user-loaded style file; takes precedence

        // Layout
        std::string activeLayout = "Default";

        // IBL / Environment
        float iblIntensity    = 1.0f;
        float skyboxIntensity = 1.0f;
        std::string skyboxPath = "textures/environment.hdr";

        // Editor camera
        float cameraFlySpeed      = 5.0f;
        float cameraFOV           = 70.0f;
        float cameraNearClip      = 0.1f;
        float cameraFarClip       = 10000.0f;
        float cameraRotationSpeed = 20000.0f;
        float cameraPanSpeed      = 200.0f;
        float cameraZoomSpeed     = 100.0f;
        float cameraShiftMult     = 3.0f;

        // Editor panels
        float thumbnailSize             = 64.0f;
        float texturePreviewFooterHeight = 220.0f;   // pinned-footer height in TextureEditor / Material / Model inspectors
        bool  showControlsOverlay       = true;
        bool  showBoneDebug             = false;
        bool  showLightGizmos           = true;
        bool  showCameraGizmos          = true;
        bool  showAABBGizmos            = false;
        bool  showGrid                  = true;
        bool  showTriIndicatorOverlay   = true;
        // Last-selected debug render mode (Normals=3 / EntityID=4 in ShadeMode).
        // Stored as u8 to avoid pulling RenderingSystem.h into this header.
        u8    lastDebugMode             = 3;

        // Selection outline (default mirrors EditorColors::SelectionOutline so existing scenes
        // look unchanged before the user edits the values).
        Vec4  outlineColor          = { 1.0f, 0.6f, 0.0f, 1.0f };
        float outlineWidth          = 1.5f;
        float outlineOccludedAlpha  = 0.65f;

        // Editor grid (defaults mirror the literals previously baked into GridPass.cpp).
        Vec4  gridAxisXColor    = { 0.80f, 0.10f, 0.15f, 1.00f };
        Vec4  gridAxisZColor    = { 0.10f, 0.25f, 0.80f, 1.00f };
        Vec4  gridColor         = { 0.41f, 0.41f, 0.41f, 0.50f };
        float gridMajorScale    = 1.0f;
        float gridFadeStart     = 20.0f;
        float gridFadeEnd       = 200.0f;
        float gridLineThickness = 1.0f;

        // Play mode
        bool  previewAnimationInEditor = true;   // Animations tick in Editing state

        // Autosave (side-channel backup; never overwrites canonical scene)
        bool  autoSaveEnabled     = true;
        float autoSaveIntervalSec = 60.0f;
        u32   autoSaveKeepN       = 10;

        // ProjectPanel thumbnails (cache + disk persist at <project>/.luth/thumbnails/).
        // thumbnailMaxDiskEntries = 0 → unbounded (matches Unity / Unreal / Godot
        // which rely on orphan cleanup only). 10000 is a safety floor against
        // runaway accumulation if orphan GC ever regresses.
        bool  thumbnailsEnabled       = true;
        u32   thumbnailMaxDiskEntries = 10000;

        // Scene persistence
        std::string lastSceneUUID;

        // Per-panel visibility (Window menu). Keyed by Panel::GetWindowID().
        // Missing keys default to true (panel shown) — matches Panel::m_Open default.
        std::unordered_map<std::string, bool> panelOpen;

        static EditorSettings Load(const std::filesystem::path& path);
        static void Save(const EditorSettings& settings, const std::filesystem::path& path);
    };
}
