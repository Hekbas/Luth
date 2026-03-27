#pragma once

#include <string>
#include <filesystem>

namespace Luth
{
    struct EditorSettings
    {
        // Style
        std::string activeStyle = "Rider";

        // Layout
        std::string activeLayout = "Default";

        // IBL / Environment
        float iblIntensity    = 1.0f;
        float skyboxIntensity = 1.0f;

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
        float thumbnailSize   = 64.0f;

        // Scene persistence
        std::string lastSceneUUID;

        static EditorSettings Load(const std::filesystem::path& path);
        static void Save(const EditorSettings& settings, const std::filesystem::path& path);
    };
}
