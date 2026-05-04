#include "lepch.h"
#include "luthien/panels/EditorSettingsWindow.h"

#include "luthien/Editor.h"
#include "luthien/widgets/Properties.h"
#include "luthien/widgets/Icons.h"

#include <imgui.h>
#include <algorithm>
#include <cstring>
#include <string>

namespace Luth
{
    namespace
    {
        // Section list lives in one place so both panes stay in sync.
        constexpr const char* kSections[] = {
            "General", "Camera", "Viewport", "Grid", "Gizmos",
            "IBL & Skybox", "Animation", "Autosave", "Thumbnails"
        };

        bool ContainsCI(const char* haystack, const char* needle)
        {
            if (!needle || !*needle) return true;
            std::string h = haystack ? haystack : "";
            std::string n = needle;
            std::transform(h.begin(), h.end(), h.begin(), ::tolower);
            std::transform(n.begin(), n.end(), n.begin(), ::tolower);
            return h.find(n) != std::string::npos;
        }
    }

    void EditorSettingsWindow::Draw()
    {
        if (!s_Open) return;

        // Center on the engine window the first time the window appears.
        ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(820, 560), ImGuiCond_FirstUseEver);

        if (!ImGui::Begin(ICON_FA_GEAR "  Preferences", &s_Open, ImGuiWindowFlags_NoDocking))
        {
            ImGui::End();
            return;
        }

        EditorSettings& s   = Editor::GetSettings();
        bool committedAny   = false;

        // Top: search bar (right-aligned)
        static char searchBuf[128] = "";
        const float searchW = 260.0f;
        const float topAvail = ImGui::GetContentRegionAvail().x;
        if (topAvail > searchW + 8.0f)
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + topAvail - searchW);
        ImGui::SetNextItemWidth(searchW);
        ImGui::InputTextWithHint("##PrefsSearch", ICON_FA_MAGNIFYING_GLASS "  Search...", searchBuf, sizeof(searchBuf));
        ImGui::Separator();

        const bool isSearching = (searchBuf[0] != '\0');
        const char* filter     = isSearching ? searchBuf : nullptr;

        // ── Two-pane layout ──
        static int  s_SelectedSection = 0;
        const float leftPaneW = 200.0f;

        // LEFT: section list (greyed when searching — list bypassed)
        ImGui::BeginChild("##PrefsLeft", ImVec2(leftPaneW, 0),
                          ImGuiChildFlags_Borders | ImGuiChildFlags_ResizeX);
        if (isSearching) ImGui::BeginDisabled();
        for (int i = 0; i < IM_ARRAYSIZE(kSections); ++i) {
            if (ImGui::Selectable(kSections[i], s_SelectedSection == i))
                s_SelectedSection = i;
        }
        if (isSearching) ImGui::EndDisabled();
        ImGui::EndChild();

        ImGui::SameLine();

        // RIGHT: section content
        ImGui::BeginChild("##PrefsRight", ImVec2(0, 0), ImGuiChildFlags_Borders);

        // Returns true if the section emitted any property row (so we can hide
        // empty sections in search mode).
        auto Header = [](const char* title) {
            ImGui::PushFont(Editor::GetMainFont());
            ImGui::TextUnformatted(title);
            ImGui::PopFont();
            ImGui::Separator();
        };

        // Property helper that respects the search filter.
        auto Match = [&](const char* label) -> bool {
            return !filter || ContainsCI(label, filter);
        };

        // Each section body is wrapped so we can render either ALL of them
        // (search mode, every section visible with header) or just the selected one.
        auto DrawSection = [&](int idx) -> int {
            int rowsRendered = 0;
            switch (idx) {
            case 0: { // General
                Header("General");
                ImGui::TextDisabled("Theme:  %s", s.activeStyle.c_str());
                ImGui::TextDisabled("Layout: %s", s.activeLayout.c_str());
                ImGui::TextWrapped("Manage themes via View > Style and layouts via View > Layouts.");
                rowsRendered = 1;  // section is informational; treat as always shown
                break;
            }
            case 1: { // Camera
                Header("Editor Camera");
                if (UI::BeginProperties("PrefsCamera")) {
                    if (Match("Fly Speed"))        { committedAny |= UI::Property("Fly Speed",        s.cameraFlySpeed,      0.1f, 0.1f, 200.0f).committed; ++rowsRendered; }
                    if (Match("FOV"))              { committedAny |= UI::Property("FOV",              s.cameraFOV,           1.0f, 30.0f, 120.0f).committed; ++rowsRendered; }
                    if (Match("Near Clip"))        { committedAny |= UI::Property("Near Clip",        s.cameraNearClip,      0.01f, 0.01f, 10.0f).committed; ++rowsRendered; }
                    if (Match("Far Clip"))         { committedAny |= UI::Property("Far Clip",         s.cameraFarClip,       1.0f, 100.0f, 50000.0f).committed; ++rowsRendered; }
                    if (Match("Rotation Speed"))   { committedAny |= UI::Property("Rotation Speed",   s.cameraRotationSpeed, 10.0f, 1000.0f, 50000.0f).committed; ++rowsRendered; }
                    if (Match("Pan Speed"))        { committedAny |= UI::Property("Pan Speed",        s.cameraPanSpeed,      1.0f, 10.0f, 1000.0f).committed; ++rowsRendered; }
                    if (Match("Zoom Speed"))       { committedAny |= UI::Property("Zoom Speed",       s.cameraZoomSpeed,     1.0f, 10.0f, 500.0f).committed; ++rowsRendered; }
                    if (Match("Shift Multiplier")) { committedAny |= UI::Property("Shift Multiplier", s.cameraShiftMult,     0.1f, 1.0f, 10.0f).committed; ++rowsRendered; }
                    UI::EndProperties();
                }
                break;
            }
            case 2: { // Viewport
                Header("Viewport");
                if (UI::BeginProperties("PrefsViewport")) {
                    if (Match("Show Controls Overlay"))  { committedAny |= UI::Property("Show Controls Overlay", s.showControlsOverlay).committed; ++rowsRendered; }
                    if (Match("Show Grid"))              { committedAny |= UI::Property("Show Grid",             s.showGrid).committed; ++rowsRendered; }
                    if (Match("Show Tri Indicator"))     { committedAny |= UI::Property("Show Tri Indicator",    s.showTriIndicatorOverlay).committed; ++rowsRendered; }
                    if (Match("Outline Color"))          { committedAny |= UI::PropertyColor("Outline Color",    s.outlineColor).committed; ++rowsRendered; }
                    if (Match("Outline Width"))          { committedAny |= UI::Property("Outline Width",         s.outlineWidth, 0.1f, 0.1f, 10.0f).committed; ++rowsRendered; }
                    if (Match("Outline Occluded Alpha")) { committedAny |= UI::Property("Outline Occluded Alpha",s.outlineOccludedAlpha, 0.01f, 0.0f, 1.0f).committed; ++rowsRendered; }
                    UI::EndProperties();
                }
                break;
            }
            case 3: { // Grid
                Header("Grid");
                if (UI::BeginProperties("PrefsGrid")) {
                    if (Match("Axis X Color"))    { committedAny |= UI::PropertyColor("Axis X Color", s.gridAxisXColor).committed; ++rowsRendered; }
                    if (Match("Axis Z Color"))    { committedAny |= UI::PropertyColor("Axis Z Color", s.gridAxisZColor).committed; ++rowsRendered; }
                    if (Match("Grid Color"))      { committedAny |= UI::PropertyColor("Grid Color",   s.gridColor).committed; ++rowsRendered; }
                    if (Match("Major Scale"))     { committedAny |= UI::Property("Major Scale",       s.gridMajorScale,    0.1f,  0.1f, 100.0f).committed; ++rowsRendered; }
                    if (Match("Fade Start"))      { committedAny |= UI::Property("Fade Start",        s.gridFadeStart,     1.0f,  1.0f, 1000.0f).committed; ++rowsRendered; }
                    if (Match("Fade End"))        { committedAny |= UI::Property("Fade End",          s.gridFadeEnd,       1.0f,  1.0f, 5000.0f).committed; ++rowsRendered; }
                    if (Match("Line Thickness"))  { committedAny |= UI::Property("Line Thickness",    s.gridLineThickness, 0.05f, 0.1f, 10.0f).committed; ++rowsRendered; }
                    UI::EndProperties();
                }
                break;
            }
            case 4: { // Gizmos
                Header("Gizmos");
                if (UI::BeginProperties("PrefsGizmos")) {
                    if (Match("Light Gizmos"))  { committedAny |= UI::Property("Light Gizmos",  s.showLightGizmos).committed; ++rowsRendered; }
                    if (Match("Camera Gizmos")) { committedAny |= UI::Property("Camera Gizmos", s.showCameraGizmos).committed; ++rowsRendered; }
                    if (Match("AABB Gizmos"))   { committedAny |= UI::Property("AABB Gizmos",   s.showAABBGizmos).committed; ++rowsRendered; }
                    if (Match("Bone Debug"))    { committedAny |= UI::Property("Bone Debug",    s.showBoneDebug).committed; ++rowsRendered; }
                    UI::EndProperties();
                }
                break;
            }
            case 5: { // IBL & Skybox
                Header("IBL & Skybox");
                if (UI::BeginProperties("PrefsIBL")) {
                    if (Match("IBL Intensity"))    { committedAny |= UI::Property("IBL Intensity",    s.iblIntensity,    0.01f, 0.0f, 8.0f).committed; ++rowsRendered; }
                    if (Match("Skybox Intensity")) { committedAny |= UI::Property("Skybox Intensity", s.skyboxIntensity, 0.01f, 0.0f, 8.0f).committed; ++rowsRendered; }
                    UI::EndProperties();
                }
                if (rowsRendered > 0 || !filter) {
                    ImGui::TextDisabled("HDR: %s", s.skyboxPath.c_str());
                    ImGui::TextDisabled("Browse via Scene toolbar > Environment.");
                }
                break;
            }
            case 6: { // Animation
                Header("Animation");
                if (UI::BeginProperties("PrefsAnim")) {
                    if (Match("Preview Animation In Editor"))
                        { committedAny |= UI::Property("Preview Animation In Editor", s.previewAnimationInEditor).committed; ++rowsRendered; }
                    UI::EndProperties();
                }
                break;
            }
            case 7: { // Autosave
                Header("Autosave");
                if (UI::BeginProperties("PrefsAutosave")) {
                    if (Match("Enabled"))        { committedAny |= UI::Property("Enabled",        s.autoSaveEnabled).committed; ++rowsRendered; }
                    if (Match("Interval (sec)")) { committedAny |= UI::Property("Interval (sec)", s.autoSaveIntervalSec, 1.0f, 5.0f, 3600.0f).committed; ++rowsRendered; }
                    if (Match("Keep N Backups")) {
                        int keepN = static_cast<int>(s.autoSaveKeepN);
                        if (UI::Property("Keep N Backups", keepN, 1, 100).committed) {
                            s.autoSaveKeepN = static_cast<u32>(keepN);
                            committedAny = true;
                        }
                        ++rowsRendered;
                    }
                    UI::EndProperties();
                }
                break;
            }
            case 8: { // Thumbnails
                Header("Thumbnails");
                if (UI::BeginProperties("PrefsThumbs")) {
                    if (Match("Enabled"))   { committedAny |= UI::Property("Enabled",   s.thumbnailsEnabled).committed; ++rowsRendered; }
                    if (Match("Size (px)")) { committedAny |= UI::Property("Size (px)", s.thumbnailSize, 1.0f, 16.0f, 256.0f).committed; ++rowsRendered; }
                    if (Match("Max Disk Entries")) {
                        int diskMax = static_cast<int>(s.thumbnailMaxDiskEntries);
                        if (UI::Property("Max Disk Entries (0 = unbounded)", diskMax, 0, 100000).committed) {
                            s.thumbnailMaxDiskEntries = static_cast<u32>(diskMax);
                            committedAny = true;
                        }
                        ++rowsRendered;
                    }
                    UI::EndProperties();
                }
                break;
            }
            }
            return rowsRendered;
        };

        if (isSearching) {
            // Render every section; spacer between non-empty ones for breathing room.
            int totalRows = 0;
            for (int i = 0; i < IM_ARRAYSIZE(kSections); ++i) {
                int before = totalRows;
                totalRows += DrawSection(i);
                if (totalRows > before) ImGui::Spacing();
            }
            if (totalRows == 0)
                ImGui::TextDisabled("No settings match \"%s\".", searchBuf);
        }
        else {
            DrawSection(s_SelectedSection);
        }

        ImGui::EndChild();
        ImGui::End();

        if (committedAny)
        {
            Editor::SaveSettings();
            Editor::ApplyPersistence();
        }
    }
}
