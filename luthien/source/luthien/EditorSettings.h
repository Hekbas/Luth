#pragma once

#include <string>
#include <filesystem>

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
        float thumbnailSize        = 64.0f;
        bool  showControlsOverlay  = true;
        bool  showBoneDebug        = false;
        bool  showLightGizmos      = true;
        bool  showCameraGizmos     = true;
        bool  showAABBGizmos       = false;
        bool  showGrid             = true;

        // Play mode
        bool  previewAnimationInEditor = true;   // Animations tick in Editing state

        // Scene persistence
        std::string lastSceneUUID;

        static EditorSettings Load(const std::filesystem::path& path);
        static void Save(const EditorSettings& settings, const std::filesystem::path& path);
    };
}
