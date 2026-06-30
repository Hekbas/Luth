#include "lepch.h"
#include "luthien/EditorSettings.h"

#include <nlohmann/json.hpp>
#include <fstream>

namespace Luth
{
    using json = nlohmann::json;

    namespace
    {
        // Vec4 ↔ JSON array. Skips assignment on malformed entries so a corrupt file leaves struct defaults intact.
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
            LH_LOG(Editor, warn, "EditorSettings file not found at '{}', using defaults", path.string());
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
            settings.enableVolumetricFog = j.value("enableVolumetricFog", settings.enableVolumetricFog);
            settings.cameraFlySpeed      = j.value("cameraFlySpeed", settings.cameraFlySpeed);
            settings.cameraFOV           = j.value("cameraFOV", settings.cameraFOV);
            settings.cameraNearClip      = j.value("cameraNearClip", settings.cameraNearClip);
            settings.cameraFarClip       = j.value("cameraFarClip", settings.cameraFarClip);
            settings.cameraRotationSpeed = j.value("cameraRotationSpeed", settings.cameraRotationSpeed);
            settings.cameraPanSpeed      = j.value("cameraPanSpeed", settings.cameraPanSpeed);
            settings.cameraZoomSpeed     = j.value("cameraZoomSpeed", settings.cameraZoomSpeed);
            settings.cameraShiftMult     = j.value("cameraShiftMult", settings.cameraShiftMult);
            settings.thumbnailSize              = j.value("thumbnailSize", settings.thumbnailSize);
            settings.texturePreviewFooterHeight = j.value("texturePreviewFooterHeight", settings.texturePreviewFooterHeight);
            settings.showControlsOverlay = j.value("showControlsOverlay", settings.showControlsOverlay);
            settings.lightsSelected  = j.value("gizmoLightsSelected",  settings.lightsSelected);
            settings.lightsAll       = j.value("gizmoLightsAll",       settings.lightsAll);
            settings.camerasSelected = j.value("gizmoCamerasSelected", settings.camerasSelected);
            settings.camerasAll      = j.value("gizmoCamerasAll",      settings.camerasAll);
            settings.boundsSelected  = j.value("gizmoBoundsSelected",  settings.boundsSelected);
            settings.boundsAll       = j.value("gizmoBoundsAll",       settings.boundsAll);
            settings.bonesSelected   = j.value("gizmoBonesSelected",   settings.bonesSelected);
            settings.bonesAll        = j.value("gizmoBonesAll",        settings.bonesAll);
            settings.fogSelected     = j.value("gizmoFogSelected",     settings.fogSelected);
            settings.fogAll          = j.value("gizmoFogAll",          settings.fogAll);
            settings.windSelected    = j.value("gizmoWindSelected",    settings.windSelected);
            settings.windAll         = j.value("gizmoWindAll",         settings.windAll);
            settings.gizmoAlphaUnselected = j.value("gizmoAlphaUnselected", settings.gizmoAlphaUnselected);
            settings.gizmoIconScale       = j.value("gizmoIconScale", settings.gizmoIconScale);
            settings.showGrid            = j.value("showGrid", settings.showGrid);
            settings.showTriIndicatorOverlay = j.value("showTriIndicatorOverlay", settings.showTriIndicatorOverlay);
            settings.lastDebugMode       = (u8)j.value("lastDebugMode", (int)settings.lastDebugMode);
            settings.renderPanelTab      = j.value("renderPanelTab", settings.renderPanelTab);
            settings.renderDenoiserTab   = j.value("renderDenoiserTab", settings.renderDenoiserTab);

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

            settings.physicsShapesSelected = j.value("physicsShapesSelected", settings.physicsShapesSelected);
            settings.physicsShapesAll      = j.value("physicsShapesAll",      settings.physicsShapesAll);
            settings.physicsAABBsSelected  = j.value("physicsAABBsSelected",  settings.physicsAABBsSelected);
            settings.physicsAABBsAll       = j.value("physicsAABBsAll",       settings.physicsAABBsAll);
            settings.physicsCoMSelected    = j.value("physicsCoMSelected",    settings.physicsCoMSelected);
            settings.physicsCoMAll         = j.value("physicsCoMAll",         settings.physicsCoMAll);
            settings.physicsColorMode      = static_cast<PhysicsDebugColorMode>(
                                                 j.value("physicsColorMode", static_cast<int>(settings.physicsColorMode)));
            LoadVec4(j, "physicsUniformColor", settings.physicsUniformColor);
            settings.physicsDebugSegments  = j.value("physicsDebugSegments",  settings.physicsDebugSegments);
            settings.physicsAlphaUnselected = j.value("physicsAlphaUnselected", settings.physicsAlphaUnselected);

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

            LH_LOG(Editor, info, "Loaded editor settings from '{}'", path.string());
        }
        catch (const std::exception& e)
        {
            LH_LOG(Editor, error, "Failed to load editor settings: {}", e.what());
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
            j["enableVolumetricFog"] = settings.enableVolumetricFog;
            j["skyboxPath"]      = settings.skyboxPath;
            j["cameraFlySpeed"]      = settings.cameraFlySpeed;
            j["cameraFOV"]           = settings.cameraFOV;
            j["cameraNearClip"]      = settings.cameraNearClip;
            j["cameraFarClip"]       = settings.cameraFarClip;
            j["cameraRotationSpeed"] = settings.cameraRotationSpeed;
            j["cameraPanSpeed"]      = settings.cameraPanSpeed;
            j["cameraZoomSpeed"]     = settings.cameraZoomSpeed;
            j["cameraShiftMult"]     = settings.cameraShiftMult;
            j["thumbnailSize"]              = settings.thumbnailSize;
            j["texturePreviewFooterHeight"] = settings.texturePreviewFooterHeight;
            j["showControlsOverlay"] = settings.showControlsOverlay;
            j["gizmoLightsSelected"]  = settings.lightsSelected;
            j["gizmoLightsAll"]       = settings.lightsAll;
            j["gizmoCamerasSelected"] = settings.camerasSelected;
            j["gizmoCamerasAll"]      = settings.camerasAll;
            j["gizmoBoundsSelected"]  = settings.boundsSelected;
            j["gizmoBoundsAll"]       = settings.boundsAll;
            j["gizmoBonesSelected"]   = settings.bonesSelected;
            j["gizmoBonesAll"]        = settings.bonesAll;
            j["gizmoFogSelected"]     = settings.fogSelected;
            j["gizmoFogAll"]          = settings.fogAll;
            j["gizmoWindSelected"]    = settings.windSelected;
            j["gizmoWindAll"]         = settings.windAll;
            j["gizmoAlphaUnselected"] = settings.gizmoAlphaUnselected;
            j["gizmoIconScale"]       = settings.gizmoIconScale;
            j["showGrid"]            = settings.showGrid;
            j["showTriIndicatorOverlay"] = settings.showTriIndicatorOverlay;
            j["lastDebugMode"]       = (int)settings.lastDebugMode;
            j["renderPanelTab"]      = settings.renderPanelTab;
            j["renderDenoiserTab"]   = settings.renderDenoiserTab;

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

            j["physicsShapesSelected"]  = settings.physicsShapesSelected;
            j["physicsShapesAll"]       = settings.physicsShapesAll;
            j["physicsAABBsSelected"]   = settings.physicsAABBsSelected;
            j["physicsAABBsAll"]        = settings.physicsAABBsAll;
            j["physicsCoMSelected"]     = settings.physicsCoMSelected;
            j["physicsCoMAll"]          = settings.physicsCoMAll;
            j["physicsColorMode"]       = static_cast<int>(settings.physicsColorMode);
            j["physicsUniformColor"]    = ToJson(settings.physicsUniformColor);
            j["physicsDebugSegments"]   = settings.physicsDebugSegments;
            j["physicsAlphaUnselected"] = settings.physicsAlphaUnselected;

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

            LH_LOG(Editor, info, "Saved editor settings to '{}'", path.string());
        }
        catch (const std::exception& e)
        {
            LH_LOG(Editor, error, "Failed to save editor settings: {}", e.what());
        }
    }
}
