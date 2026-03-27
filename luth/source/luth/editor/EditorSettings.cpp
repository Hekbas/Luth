#include "luthpch.h"
#include "luth/editor/EditorSettings.h"

#include <nlohmann/json.hpp>
#include <fstream>

namespace Luth
{
    using json = nlohmann::json;

    EditorSettings EditorSettings::Load(const std::filesystem::path& path)
    {
        EditorSettings settings;

        if (!std::filesystem::exists(path))
        {
            LH_CORE_WARN("EditorSettings file not found at '{}', using defaults", path.string());
            return settings;
        }

        try
        {
            std::ifstream file(path);
            json j = json::parse(file);

            settings.activeStyle    = j.value("activeStyle", settings.activeStyle);
            settings.activeLayout   = j.value("activeLayout", settings.activeLayout);
            settings.iblIntensity    = j.value("iblIntensity", settings.iblIntensity);
            settings.skyboxIntensity = j.value("skyboxIntensity", settings.skyboxIntensity);
            settings.cameraFlySpeed      = j.value("cameraFlySpeed", settings.cameraFlySpeed);
            settings.cameraFOV           = j.value("cameraFOV", settings.cameraFOV);
            settings.cameraNearClip      = j.value("cameraNearClip", settings.cameraNearClip);
            settings.cameraFarClip       = j.value("cameraFarClip", settings.cameraFarClip);
            settings.cameraRotationSpeed = j.value("cameraRotationSpeed", settings.cameraRotationSpeed);
            settings.cameraPanSpeed      = j.value("cameraPanSpeed", settings.cameraPanSpeed);
            settings.cameraZoomSpeed     = j.value("cameraZoomSpeed", settings.cameraZoomSpeed);
            settings.cameraShiftMult     = j.value("cameraShiftMult", settings.cameraShiftMult);
            settings.thumbnailSize   = j.value("thumbnailSize", settings.thumbnailSize);
            settings.lastSceneUUID   = j.value("lastSceneUUID", settings.lastSceneUUID);

            LH_CORE_INFO("Loaded editor settings from '{}'", path.string());
        }
        catch (const std::exception& e)
        {
            LH_CORE_ERROR("Failed to load editor settings: {}", e.what());
        }

        return settings;
    }

    void EditorSettings::Save(const EditorSettings& settings, const std::filesystem::path& path)
    {
        try
        {
            json j;
            j["activeStyle"]     = settings.activeStyle;
            j["activeLayout"]    = settings.activeLayout;
            j["iblIntensity"]    = settings.iblIntensity;
            j["skyboxIntensity"] = settings.skyboxIntensity;
            j["cameraFlySpeed"]      = settings.cameraFlySpeed;
            j["cameraFOV"]           = settings.cameraFOV;
            j["cameraNearClip"]      = settings.cameraNearClip;
            j["cameraFarClip"]       = settings.cameraFarClip;
            j["cameraRotationSpeed"] = settings.cameraRotationSpeed;
            j["cameraPanSpeed"]      = settings.cameraPanSpeed;
            j["cameraZoomSpeed"]     = settings.cameraZoomSpeed;
            j["cameraShiftMult"]     = settings.cameraShiftMult;
            j["thumbnailSize"]   = settings.thumbnailSize;
            j["lastSceneUUID"]   = settings.lastSceneUUID;

            std::ofstream file(path);
            file << j.dump(4);

            LH_CORE_INFO("Saved editor settings to '{}'", path.string());
        }
        catch (const std::exception& e)
        {
            LH_CORE_ERROR("Failed to save editor settings: {}", e.what());
        }
    }
}
