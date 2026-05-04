#include "lepch.h"
#include "luthien/panels/EditorSettingsWindow.h"

#include "luthien/Editor.h"
#include "luthien/widgets/Properties.h"
#include "luthien/widgets/Icons.h"

#include <imgui.h>

namespace Luth
{
    void EditorSettingsWindow::Draw()
    {
        if (!s_Open) return;

        ImGui::SetNextWindowSize(ImVec2(520, 640), ImGuiCond_FirstUseEver);
        if (!ImGui::Begin(ICON_FA_GEAR "  Preferences", &s_Open, ImGuiWindowFlags_NoDocking))
        {
            ImGui::End();
            return;
        }

        EditorSettings& s = Editor::GetSettings();
        bool committedAny = false;

        if (ImGui::CollapsingHeader("General", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::TextDisabled("Theme:  %s", s.activeStyle.c_str());
            ImGui::TextDisabled("Layout: %s", s.activeLayout.c_str());
            ImGui::TextWrapped("Manage themes via View > Style and layouts via View > Layouts.");
        }

        if (ImGui::CollapsingHeader("Editor Camera", ImGuiTreeNodeFlags_DefaultOpen))
        {
            if (UI::BeginProperties("PrefsCamera"))
            {
                committedAny |= UI::Property("Fly Speed",        s.cameraFlySpeed,      0.1f, 0.1f, 200.0f).committed;
                committedAny |= UI::Property("FOV",              s.cameraFOV,           1.0f, 30.0f, 120.0f).committed;
                committedAny |= UI::Property("Near Clip",        s.cameraNearClip,      0.01f, 0.01f, 10.0f).committed;
                committedAny |= UI::Property("Far Clip",         s.cameraFarClip,       1.0f, 100.0f, 50000.0f).committed;
                committedAny |= UI::Property("Rotation Speed",   s.cameraRotationSpeed, 10.0f, 1000.0f, 50000.0f).committed;
                committedAny |= UI::Property("Pan Speed",        s.cameraPanSpeed,      1.0f, 10.0f, 1000.0f).committed;
                committedAny |= UI::Property("Zoom Speed",       s.cameraZoomSpeed,     1.0f, 10.0f, 500.0f).committed;
                committedAny |= UI::Property("Shift Multiplier", s.cameraShiftMult,     0.1f, 1.0f, 10.0f).committed;
                UI::EndProperties();
            }
        }

        if (ImGui::CollapsingHeader("Viewport"))
        {
            if (UI::BeginProperties("PrefsViewport"))
            {
                committedAny |= UI::Property("Show Controls Overlay", s.showControlsOverlay).committed;
                committedAny |= UI::Property("Show Grid",             s.showGrid).committed;
                committedAny |= UI::PropertyColor("Outline Color",    s.outlineColor).committed;
                committedAny |= UI::Property("Outline Width",         s.outlineWidth, 0.1f, 0.1f, 10.0f).committed;
                committedAny |= UI::Property("Outline Occluded Alpha",s.outlineOccludedAlpha, 0.01f, 0.0f, 1.0f).committed;
                UI::EndProperties();
            }
        }

        if (ImGui::CollapsingHeader("Grid"))
        {
            if (UI::BeginProperties("PrefsGrid"))
            {
                committedAny |= UI::PropertyColor("Axis X Color", s.gridAxisXColor).committed;
                committedAny |= UI::PropertyColor("Axis Z Color", s.gridAxisZColor).committed;
                committedAny |= UI::PropertyColor("Grid Color",   s.gridColor).committed;
                committedAny |= UI::Property("Major Scale",       s.gridMajorScale,    0.1f,  0.1f, 100.0f).committed;
                committedAny |= UI::Property("Fade Start",        s.gridFadeStart,     1.0f,  1.0f, 1000.0f).committed;
                committedAny |= UI::Property("Fade End",          s.gridFadeEnd,       1.0f,  1.0f, 5000.0f).committed;
                committedAny |= UI::Property("Line Thickness",    s.gridLineThickness, 0.05f, 0.1f, 10.0f).committed;
                UI::EndProperties();
            }
        }

        if (ImGui::CollapsingHeader("Gizmos"))
        {
            if (UI::BeginProperties("PrefsGizmos"))
            {
                committedAny |= UI::Property("Light Gizmos",  s.showLightGizmos).committed;
                committedAny |= UI::Property("Camera Gizmos", s.showCameraGizmos).committed;
                committedAny |= UI::Property("AABB Gizmos",   s.showAABBGizmos).committed;
                committedAny |= UI::Property("Bone Debug",    s.showBoneDebug).committed;
                UI::EndProperties();
            }
        }

        if (ImGui::CollapsingHeader("IBL / Skybox"))
        {
            if (UI::BeginProperties("PrefsIBL"))
            {
                committedAny |= UI::Property("IBL Intensity",    s.iblIntensity,    0.01f, 0.0f, 8.0f).committed;
                committedAny |= UI::Property("Skybox Intensity", s.skyboxIntensity, 0.01f, 0.0f, 8.0f).committed;
                UI::EndProperties();
            }
            ImGui::TextDisabled("HDR: %s", s.skyboxPath.c_str());
            ImGui::TextDisabled("Browse via Scene toolbar > Environment.");
        }

        if (ImGui::CollapsingHeader("Animation"))
        {
            if (UI::BeginProperties("PrefsAnim"))
            {
                committedAny |= UI::Property("Preview Animation In Editor", s.previewAnimationInEditor).committed;
                UI::EndProperties();
            }
        }

        if (ImGui::CollapsingHeader("Autosave"))
        {
            if (UI::BeginProperties("PrefsAutosave"))
            {
                committedAny |= UI::Property("Enabled",        s.autoSaveEnabled).committed;
                committedAny |= UI::Property("Interval (sec)", s.autoSaveIntervalSec, 1.0f, 5.0f, 3600.0f).committed;
                int keepN = static_cast<int>(s.autoSaveKeepN);
                if (UI::Property("Keep N Backups", keepN, 1, 100).committed) {
                    s.autoSaveKeepN = static_cast<u32>(keepN);
                    committedAny = true;
                }
                UI::EndProperties();
            }
        }

        if (ImGui::CollapsingHeader("Thumbnails"))
        {
            if (UI::BeginProperties("PrefsThumbs"))
            {
                committedAny |= UI::Property("Enabled",   s.thumbnailsEnabled).committed;
                committedAny |= UI::Property("Size (px)", s.thumbnailSize, 1.0f, 16.0f, 256.0f).committed;
                int diskMax = static_cast<int>(s.thumbnailMaxDiskEntries);
                if (UI::Property("Max Disk Entries (0 = unbounded)", diskMax, 0, 100000).committed) {
                    s.thumbnailMaxDiskEntries = static_cast<u32>(diskMax);
                    committedAny = true;
                }
                UI::EndProperties();
            }
        }

        ImGui::End();

        if (committedAny)
        {
            Editor::SaveSettings();
            Editor::ApplyPersistence();
        }
    }
}
