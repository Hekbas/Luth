#include "lepch.h"
#include "luthien/EditorSettings.h"

#include <nlohmann/json.hpp>
#include <fstream>

namespace Luth
{
    using json = nlohmann::json;

    namespace
    {
        // Vec4 ↔ JSON array. Skips assignment on malformed entries so a corrupt
        // file leaves struct defaults intact.
        void LoadVec4(const json& j, const char* key, Vec4& out)
        {
            if (j.contains(key) && j[key].is_array() && j[key].size() == 4)
                out = Vec4(j[key][0].get<float>(), j[key][1].get<float>(),
                           j[key][2].get<float>(), j[key][3].get<float>());
        }
        json ToJson(const Vec4& v)
        {
            return json::array({ v.r, v.g, v.b, v.a });
        }
    }

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

            settings.activeStyle     = j.value("activeStyle", settings.activeStyle);
            settings.activeStylePath = j.value("activeStylePath", settings.activeStylePath);
            settings.activeLayout    = j.value("activeLayout", settings.activeLayout);
            settings.iblIntensity    = j.value("iblIntensity", settings.iblIntensity);
            settings.skyboxIntensity = j.value("skyboxIntensity", settings.skyboxIntensity);
            settings.skyboxPath      = j.value("skyboxPath", settings.skyboxPath);
            settings.cameraFlySpeed      = j.value("cameraFlySpeed", settings.cameraFlySpeed);
            settings.cameraFOV           = j.value("cameraFOV", settings.cameraFOV);
            settings.cameraNearClip      = j.value("cameraNearClip", settings.cameraNearClip);
            settings.cameraFarClip       = j.value("cameraFarClip", settings.cameraFarClip);
            settings.cameraRotationSpeed = j.value("cameraRotationSpeed", settings.cameraRotationSpeed);
            settings.cameraPanSpeed      = j.value("cameraPanSpeed", settings.cameraPanSpeed);
            settings.cameraZoomSpeed     = j.value("cameraZoomSpeed", settings.cameraZoomSpeed);
            settings.cameraShiftMult     = j.value("cameraShiftMult", settings.cameraShiftMult);
            settings.thumbnailSize       = j.value("thumbnailSize", settings.thumbnailSize);
            settings.showControlsOverlay = j.value("showControlsOverlay", settings.showControlsOverlay);
            settings.showBoneDebug       = j.value("showBoneDebug", settings.showBoneDebug);
            settings.showLightGizmos     = j.value("showLightGizmos", settings.showLightGizmos);
            settings.showCameraGizmos    = j.value("showCameraGizmos", settings.showCameraGizmos);
            settings.showAABBGizmos      = j.value("showAABBGizmos", settings.showAABBGizmos);
            settings.showGrid            = j.value("showGrid", settings.showGrid);

            LoadVec4(j, "outlineColor", settings.outlineColor);
            settings.outlineWidth         = j.value("outlineWidth", settings.outlineWidth);
            settings.outlineOccludedAlpha = j.value("outlineOccludedAlpha", settings.outlineOccludedAlpha);

            LoadVec4(j, "gridAxisXColor", settings.gridAxisXColor);
            LoadVec4(j, "gridAxisZColor", settings.gridAxisZColor);
            LoadVec4(j, "gridColor",      settings.gridColor);
            settings.gridMajorScale    = j.value("gridMajorScale", settings.gridMajorScale);
            settings.gridFadeStart     = j.value("gridFadeStart", settings.gridFadeStart);
            settings.gridFadeEnd       = j.value("gridFadeEnd", settings.gridFadeEnd);
            settings.gridLineThickness = j.value("gridLineThickness", settings.gridLineThickness);

            settings.previewAnimationInEditor = j.value("previewAnimationInEditor", settings.previewAnimationInEditor);

            settings.autoSaveEnabled     = j.value("autoSaveEnabled", settings.autoSaveEnabled);
            settings.autoSaveIntervalSec = j.value("autoSaveIntervalSec", settings.autoSaveIntervalSec);
            settings.autoSaveKeepN       = j.value("autoSaveKeepN", settings.autoSaveKeepN);

            settings.thumbnailsEnabled       = j.value("thumbnailsEnabled", settings.thumbnailsEnabled);
            settings.thumbnailMaxDiskEntries = j.value("thumbnailMaxDiskEntries", settings.thumbnailMaxDiskEntries);

            settings.lastSceneUUID   = j.value("lastSceneUUID", settings.lastSceneUUID);

            if (j.contains("panel_open") && j["panel_open"].is_object())
            {
                for (auto it = j["panel_open"].begin(); it != j["panel_open"].end(); ++it)
                    if (it.value().is_boolean())
                        settings.panelOpen[it.key()] = it.value().get<bool>();
            }

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
            j["activeStylePath"] = settings.activeStylePath;
            j["activeLayout"]    = settings.activeLayout;
            j["iblIntensity"]    = settings.iblIntensity;
            j["skyboxIntensity"] = settings.skyboxIntensity;
            j["skyboxPath"]      = settings.skyboxPath;
            j["cameraFlySpeed"]      = settings.cameraFlySpeed;
            j["cameraFOV"]           = settings.cameraFOV;
            j["cameraNearClip"]      = settings.cameraNearClip;
            j["cameraFarClip"]       = settings.cameraFarClip;
            j["cameraRotationSpeed"] = settings.cameraRotationSpeed;
            j["cameraPanSpeed"]      = settings.cameraPanSpeed;
            j["cameraZoomSpeed"]     = settings.cameraZoomSpeed;
            j["cameraShiftMult"]     = settings.cameraShiftMult;
            j["thumbnailSize"]       = settings.thumbnailSize;
            j["showControlsOverlay"] = settings.showControlsOverlay;
            j["showBoneDebug"]       = settings.showBoneDebug;
            j["showLightGizmos"]     = settings.showLightGizmos;
            j["showCameraGizmos"]    = settings.showCameraGizmos;
            j["showAABBGizmos"]      = settings.showAABBGizmos;
            j["showGrid"]            = settings.showGrid;

            j["outlineColor"]         = ToJson(settings.outlineColor);
            j["outlineWidth"]         = settings.outlineWidth;
            j["outlineOccludedAlpha"] = settings.outlineOccludedAlpha;

            j["gridAxisXColor"]    = ToJson(settings.gridAxisXColor);
            j["gridAxisZColor"]    = ToJson(settings.gridAxisZColor);
            j["gridColor"]         = ToJson(settings.gridColor);
            j["gridMajorScale"]    = settings.gridMajorScale;
            j["gridFadeStart"]     = settings.gridFadeStart;
            j["gridFadeEnd"]       = settings.gridFadeEnd;
            j["gridLineThickness"] = settings.gridLineThickness;

            j["previewAnimationInEditor"] = settings.previewAnimationInEditor;

            j["autoSaveEnabled"]     = settings.autoSaveEnabled;
            j["autoSaveIntervalSec"] = settings.autoSaveIntervalSec;
            j["autoSaveKeepN"]       = settings.autoSaveKeepN;

            j["thumbnailsEnabled"]       = settings.thumbnailsEnabled;
            j["thumbnailMaxDiskEntries"] = settings.thumbnailMaxDiskEntries;

            j["lastSceneUUID"]   = settings.lastSceneUUID;

            json po = json::object();
            for (const auto& [name, open] : settings.panelOpen)
                po[name] = open;
            j["panel_open"] = po;

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
