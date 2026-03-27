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

        // Editor panels
        float cameraFlySpeed  = 5.0f;
        float thumbnailSize   = 64.0f;

        // Scene persistence
        std::string lastSceneUUID;

        static EditorSettings Load(const std::filesystem::path& path);
        static void Save(const EditorSettings& settings, const std::filesystem::path& path);
    };
}
