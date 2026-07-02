#include "lepch.h"
#include "luthien/panels/ScenePanel.h"
#include "luthien/panels/RenderPanel.h"
#include "luthien/EditorSelection.h"
#include "luthien/EditorSnapshot.h"
#include "luthien/commands/Commands.h"
#include "luthien/CommandHistory.h"
#include "luthien/EditorSettings.h"
#include "luthien/EditorColors.h"
#include "luthien/PlayModeController.h"
#include "luthien/viewport/ViewportRenderer.h"
#include "luthien/viewport/GizmoController.h"
#include "luthien/viewport/ViewportOverlays.h"
#include "luthien/panels/FrameDebuggerPanel.h"
#include "luth/platform/FileDialog.h"
#include "luth/resources/FileSystem.h"
#include "luth/scene/Components.h"
#include "luth/scene/Scene.h"
#include "luth/scene/systems/PickingSystem.h"
#include "luth/scene/systems/SystemRegistry.h"
#include "luth/renderer/Renderer.h"
#include "luth/renderer/settings/RestirGiSettings.h"
#include "luthien/widgets/ImGuiUtils.h"
#include "luthien/widgets/Icons.h"
#include "luthien/widgets/Widgets.h"
#include <ImGuizmo.h>

namespace Luth
{
    using namespace Component;

    ScenePanel::ScenePanel(RenderingSystem* renderingSystem)
        : m_RenderingSystem(renderingSystem)
        , m_Viewport(std::make_unique<ViewportRenderer>())
        , m_Gizmo(std::make_unique<GizmoController>())
        , m_Overlays(std::make_unique<ViewportOverlays>(*m_Viewport, *m_Gizmo))
    {
        m_WindowID = "Scene";
        m_EditorCamera = EditorCamera(70.0f, 1.77f, 0.1f, 10000.0f);
        m_Gizmo->SetOperation(ImGuizmo::OPERATION::TRANSLATE);

        m_Viewport->SetOnResize([this](u32 w, u32 h) {
            m_RenderingSystem->Resize(w, h);
            m_EditorCamera.SetViewportSize((float)w, (float)h);
            m_Viewport->SetSize(w, h);
        });

        LH_LOG(Editor, info, "Created Scene panel");
    }

    ScenePanel::~ScenePanel() = default;

    void ScenePanel::OnInit()
    {
        m_SelectedEntity = EditorSelection::GetSelectedEntity();
    }

    void ScenePanel::SelectEntitiesInRect(const ImVec2& a, const ImVec2& b)
    {
        if (!m_Context) return;

        const float minX = std::min(a.x, b.x), maxX = std::max(a.x, b.x);
        const float minY = std::min(a.y, b.y), maxY = std::max(a.y, b.y);

        const bool ctrl  = ImGui::IsKeyDown(ImGuiKey_LeftCtrl)  || ImGui::IsKeyDown(ImGuiKey_RightCtrl);
        const bool shift = ImGui::IsKeyDown(ImGuiKey_LeftShift) || ImGui::IsKeyDown(ImGuiKey_RightShift);
        if (!ctrl && !shift) EditorSelection::ClearSelection();   // plain drag replaces

        const Mat4 viewProj  = m_EditorCamera.GetViewProjection();
        const ImVec2* bounds = m_Viewport->GetBounds();
        const Vec2 size      = m_Viewport->GetSize();

        // Origin-based: an entity is in the box if its world-space pivot projects inside it. Same
        // projection as ViewportOverlays::ProjectToScreen so it matches the on-screen gizmo icons.
        auto view = m_Context->Registry().view<WorldTransform>();
        for (auto handle : view) {
            Entity ent(handle, m_Context.get());
            if (!ent.IsValid()) continue;

            Vec3 worldPos = Vec3(ent.GetComponent<WorldTransform>().Matrix[3]);
            Vec4 clip = viewProj * Vec4(worldPos, 1.0f);
            if (clip.w <= 0.001f) continue;   // behind the camera
            Vec3 ndc = Vec3(clip) / clip.w;
            float sx = bounds[0].x + (ndc.x * 0.5f + 0.5f) * size.x;
            float sy = bounds[0].y + (-ndc.y * 0.5f + 0.5f) * size.y;
            if (sx < minX || sx > maxX || sy < minY || sy > maxY) continue;

            if (ctrl) EditorSelection::ToggleEntity(ent);
            else      EditorSelection::AddEntity(ent);
        }
    }

    void ScenePanel::OnGather(EditorSnapshotBuilder& builder)
    {
        // Most viewport work is unavoidably ImGui-driven (ImGuizmo, ImGui::Image of the rendertarget,
        // drag-drop). Camera matrices feed RenderingSystem via EditorViewportState in App::Run.
        auto* snap = builder.Add<SceneViewportSnapshot>();
        snap->selectionVersion = EditorSelection::GetVersion();
    }

    void ScenePanel::OnDraw(const EditorSnapshot& /*snapshot*/)
    {
        LH_PROFILE_FUNCTION();
        m_Gizmo->ResetFrameState();

        // primary = last-added, drives gizmos/camera
        m_SelectedEntity = EditorSelection::GetSelectedEntity();
        // Outline color flows through EditorSettings -> EditorViewportState -> CameraParams
        // (see luth/core/App.cpp). Visibility-only toggles still go through the direct setter.
        m_RenderingSystem->SetGridVisible(Editor::GetSettings().showGrid);

        ImGui::PushFont(Editor::GetIconRegular());
        std::string scene = ICON_VIEWPORT + std::string("  Scene");

        if (BeginWindow(scene.c_str(), ImGuiWindowFlags_NoScrollbar)) {
            // Toolbar: Left (gizmos + grid) | Transport | Right (render + dropdowns)
            {
                auto& settings = Editor::GetSettings();
                const ImGuiStyle& style  = ImGui::GetStyle();
                const float toolbarWidth = ImGui::GetContentRegionAvail().x;
                const float btnSize      = ImGui::GetFrameHeight();
                // invariant: matches widgets/ButtonGroup IconBtnSize(); icon-toggle buttons add
                // 2*(FramePadding.x - FramePadding.y) to width so the glyph isn't pinched at default style {6,4}.
                const float iconBtnW     = btnSize + 2.0f * std::max(0.0f, style.FramePadding.x - style.FramePadding.y);
                const float chevW        = btnSize * 0.75f;     // matches SplitToggleButton chevron half
                const float splitW       = iconBtnW + chevW;
                const float gap          = 2.0f;
                const float groupGap     = 8.0f;
                const float wideGap      = 14.0f;               // gizmo|grid + debug|camera separation

                // Pre-compute block widths so transport/right blocks land deterministically.
                // Transport + render-mode buttons are plain ImGui::Button({btnSize,btnSize}),
                // not icon-toggles, so keep btnSize. Splits + Overlay use iconBtnW.
                // invariant: SameLine(absX) is window-relative, NOT content-relative;
                // it ignores WindowPadding.x, so the absX values must include it.
                const float windowPadX     = style.WindowPadding.x;
                const float transportW     = (4 * btnSize) + (3 * gap);
                const float renderModeW    = (5 * btnSize) + (4 * gap);   // Wire/ShadedWire/Unlit/Lit + Path Trace
                const float rightW         = renderModeW + groupGap + splitW + wideGap + splitW + gap + splitW + gap + iconBtnW;
                const float transportStart = windowPadX + (toolbarWidth - transportW) * 0.5f;
                const float rightStart     = windowPadX + (toolbarWidth - rightW);

                // LEFT: gizmo tools + grid split
                ImGui::AlignTextToFramePadding();
                static const char* kGizmoIcons[]    = { ICON_SELECT_FILL, ICON_MOVE, ICON_ROTATE, ICON_RESIZE };
                static const char* kGizmoTooltips[] = { "Select (Q)", "Translate (W)", "Rotate (E)", "Scale (R)" };
                static const bool  kGizmoFilled[]   = { true, false, false, false };   // cursor reads as filled
                static constexpr int kGizmoOps[]    = { -1, ImGuizmo::OPERATION::TRANSLATE, ImGuizmo::OPERATION::ROTATE, ImGuizmo::OPERATION::SCALE };
                int gizmoIdx = 0;
                const int currentOp = m_Gizmo->GetOperation();
                for (int i = 0; i < IM_ARRAYSIZE(kGizmoOps); ++i) if (kGizmoOps[i] == currentOp) { gizmoIdx = i; break; }
                if (UI::IconToggleGroup("GizmoTools", kGizmoIcons, kGizmoTooltips, IM_ARRAYSIZE(kGizmoOps), &gizmoIdx, kGizmoFilled))
                    m_Gizmo->SetOperation(kGizmoOps[gizmoIdx]);

                ImGui::SameLine(0, wideGap);

                UI::SplitToggleButton("Grid", ICON_GRID, "Grid",
                    &settings.showGrid,
                    [&]() {
                        ImGui::PushFont(Editor::GetMainFont());
                        if (UI::BeginProperties("GridProps")) {
                            UI::PropertyColor("Axis X Color", settings.gridAxisXColor);
                            UI::PropertyColor("Axis Z Color", settings.gridAxisZColor);
                            UI::PropertyColor("Grid Color",   settings.gridColor);
                            UI::Property("Major Scale",      settings.gridMajorScale,    0.1f, 0.1f, 100.0f);
                            UI::Property("Fade Start",       settings.gridFadeStart,     1.0f, 1.0f, 1000.0f);
                            UI::Property("Fade End",         settings.gridFadeEnd,       1.0f, 1.0f, 5000.0f);
                            UI::Property("Line Thickness",   settings.gridLineThickness, 0.05f, 0.1f, 10.0f);
                            UI::EndProperties();
                        }
                        ImGui::PopFont();
                    });

                // CENTER: transport (centered)
                ImGui::SameLine(transportStart);

                const PlayState playState = PlayModeController::GetState();
                auto TransportBtn = [&](const char* icon, const char* id, const char* tooltip, bool enabled) -> bool {
                    if (!enabled) ImGui::BeginDisabled();
                    std::string label = std::string(icon) + id;
                    ImGui::PushFont(Editor::GetIconFill());
                    bool clicked = ImGui::Button(label.c_str(), { btnSize, btnSize });
                    ImGui::PopFont();
                    if (ImGui::IsItemHovered() && enabled)
                        ImGui::SetTooltip("%s", tooltip);
                    if (!enabled) ImGui::EndDisabled();
                    return clicked;
                };

                const bool canPlay   = (playState == PlayState::Editing);
                const bool canResume = (playState == PlayState::Paused);
                if (TransportBtn(ICON_PLAY_FILL, "##Play",
                                 canResume ? "Resume" : "Play",
                                 canPlay || canResume)) {
                    if (canResume) PlayModeController::Resume();
                    else           PlayModeController::EnterPlay();
                }
                ImGui::SameLine(0, gap);
                if (TransportBtn(ICON_PAUSE_FILL, "##Pause", "Pause", playState == PlayState::Playing))
                    PlayModeController::Pause();
                ImGui::SameLine(0, gap);
                if (TransportBtn(ICON_STOP_FILL, "##Stop", "Stop", playState != PlayState::Editing))
                    PlayModeController::Stop();
                ImGui::SameLine(0, gap);
                if (TransportBtn(ICON_STEP_FORWARD_FILL, "##Step", "Step one frame",
                                 playState == PlayState::Paused))
                    PlayModeController::RequestStep();

                // RIGHT: render modes + debug split + camera split + gizmo-vis split + overlay toggle
                ImGui::SameLine(rightStart);

                struct RenderModeBtn { const char* icon; const char* tip; int mode; bool filled; };
                static const RenderModeBtn kRenderModes[] = {
                    { ICON_WIREFRAME,        "Wireframe",                                 (int)ShadeMode::Wireframe, false },
                    { ICON_WIREFRAME_SHADED, "Shaded Wireframe",                          (int)ShadeMode::WireframeShaded, true  },
                    { ICON_CIRCLE,           "Unlit",                                     (int)ShadeMode::Unlit,     false },
                    { ICON_MATERIAL_FILL,    "Lit",                                       (int)ShadeMode::Lit,       true  },
                };
                const int  curMode    = (int)m_RenderingSystem->GetShadeMode();
                const bool ptActive   = m_RenderingSystem->GetRenderMode() == RenderMode::PathTrace;
                const ImVec4 activeCol = ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive);
                // Wire/Unlit/Lit imply Raster; clicking one switches back from Path Trace. None highlight
                // while Path Trace owns the view.
                for (int i = 0; i < IM_ARRAYSIZE(kRenderModes); ++i) {
                    const bool active  = !ptActive && (curMode == kRenderModes[i].mode);
                    const bool enabled = (kRenderModes[i].mode != -1);
                    if (active) {
                        ImGui::PushStyleColor(ImGuiCol_Button, activeCol);
                        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, activeCol);
                    }
                    if (!enabled) ImGui::BeginDisabled();
                    ImGui::PushID(i);
                    if (kRenderModes[i].filled) ImGui::PushFont(Editor::GetIconFill());
                    const bool modeClicked = ImGui::Button(kRenderModes[i].icon, { btnSize, btnSize });
                    if (kRenderModes[i].filled) ImGui::PopFont();
                    if (modeClicked && enabled) {
                        m_RenderingSystem->SetRenderMode(RenderMode::Raster);
                        m_RenderingSystem->SetShadeMode((ShadeMode)kRenderModes[i].mode);
                    }
                    ImGui::PopID();
                    if (!enabled) ImGui::EndDisabled();
                    if (active) ImGui::PopStyleColor(2);
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", kRenderModes[i].tip);
                    ImGui::SameLine(0, gap);
                }

                // Path Trace is a RenderMode (not a ShadeMode): swaps the realtime path for the reference tracer.
                if (ptActive) {
                    ImGui::PushStyleColor(ImGuiCol_Button, activeCol);
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, activeCol);
                }
                if (ImGui::Button(ICON_ATOM, { btnSize, btnSize }))
                    m_RenderingSystem->SetRenderMode(ptActive ? RenderMode::Raster : RenderMode::PathTrace);
                if (ptActive) ImGui::PopStyleColor(2);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Path Trace (ground-truth reference)");

                ImGui::SameLine(0, groupGap);

                // Debug split: icon toggles current<->lastDebugMode; chevron opens the searchable picker.
                // Disabled under Path Trace (debug views are raster-only).
                if (ptActive) ImGui::BeginDisabled();
                {
                    // Debug-active = any mode that isn't one of the three primary shades (the render-mode
                    // icons own Wire/Unlit/Lit). One negation instead of re-listing every debug value.
                    const bool dbgActive = !(curMode == (int)ShadeMode::Lit
                                          || curMode == (int)ShadeMode::Unlit
                                          || curMode == (int)ShadeMode::Wireframe);
                    bool dbgState = dbgActive;
                    if (UI::SplitToggleButton("Debug", ICON_BUG_BEETLE_FILL, "Debug Render Modes", &dbgState,
                        [this]() {
                            ImGui::PushFont(Editor::GetMainFont());
                            DrawDebugModePicker();
                            ImGui::PopFont();
                        }, true))
                    {
                        m_RenderingSystem->SetShadeMode(dbgState
                            ? (ShadeMode)settings.lastDebugMode
                            : ShadeMode::Lit);
                    }
                }
                if (ptActive) ImGui::EndDisabled();

                ImGui::SameLine(0, wideGap);

                // Camera split (chevron-only).
                UI::SplitToggleButton("Camera", ICON_CAMERA_FILL, "Camera Settings", nullptr,
                    [this]() {
                        ImGui::PushFont(Editor::GetMainFont());
                        if (UI::BeginProperties("CamSpeedProps")) {
                            UI::Property("Fly Speed", m_EditorCamera.GetFlySpeedRef(), 0.1f, 0.1f, 200.0f);
                            UI::EndProperties();
                        }
                        ImGui::Separator();
                        ImGui::Text("Camera");
                        ImGui::Spacing();
                        if (UI::BeginProperties("CameraProps")) {
                            if (UI::Property("FOV", m_EditorCamera.GetFOVRef(), 1.0f, 30.0f, 120.0f))
                                m_EditorCamera.SetFOV(m_EditorCamera.GetFOV());
                            UI::Property("Near Clip", m_EditorCamera.GetNearClipRef(), 0.01f, 0.01f, 10.0f);
                            UI::Property("Far Clip",  m_EditorCamera.GetFarClipRef(),  1.0f, 100.0f, 50000.0f);
                            UI::EndProperties();
                        }
                        ImGui::Separator();
                        ImGui::Text("Controls");
                        ImGui::Spacing();
                        if (UI::BeginProperties("ControlsProps")) {
                            UI::Property("Rotation Speed",   m_EditorCamera.GetRotationSpeedRef(), 10.0f, 1000.0f, 50000.0f);
                            UI::Property("Pan Speed",        m_EditorCamera.GetPanSpeedRef(),     1.0f, 10.0f, 1000.0f);
                            UI::Property("Zoom Speed",       m_EditorCamera.GetZoomSpeedRef(),    1.0f, 10.0f, 500.0f);
                            UI::Property("Shift Multiplier", m_EditorCamera.GetShiftMultiplierRef(), 0.1f, 1.0f, 10.0f);
                            UI::EndProperties();
                        }
                        ImGui::PopFont();
                    }, true);

                ImGui::SameLine(0, gap);

                // Gizmo visibility split: icon toggles all gizmos on/off; chevron lists
                // per-gizmo flags + the tri-indicator overlay (Grid lives in its own split now).
                {
                    // All gizmo + physics visibility flags in one list so the eye toggle hides/restores
                    // them generically; Tri Indicator stays independent (drawn but not eye-gated).
                    bool* xformVisRef = m_Gizmo->GetTransformGizmoVisibleRef();
                    bool* toggles[] = {
                        xformVisRef,
                        &settings.lightsSelected,  &settings.lightsAll,
                        &settings.camerasSelected, &settings.camerasAll,
                        &settings.boundsSelected,  &settings.boundsAll,
                        &settings.bonesSelected,   &settings.bonesAll,
                        &settings.fogSelected,     &settings.fogAll,
                        &settings.windSelected,    &settings.windAll,
                        &settings.physicsShapesSelected, &settings.physicsShapesAll,
                        &settings.physicsAABBsSelected,  &settings.physicsAABBsAll,
                        &settings.physicsCoMSelected,    &settings.physicsCoMAll,
                    };
                    constexpr int kGizmoToggleCount = (int)(sizeof(toggles) / sizeof(toggles[0]));
                    static bool s_savedGizmo[kGizmoToggleCount] = {};
                    static bool s_savedGizmoValid = false;

                    bool gizState = false;
                    for (bool* t : toggles) gizState = gizState || *t;

                    if (UI::SplitToggleButton("GizmoVis", ICON_EYE, "Gizmos", &gizState,
                        [&settings, xformVisRef]() {
                            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(2, 2));
                            ImGui::PushFont(Editor::GetMainFont());
                            ImGui::Checkbox("Transform",     xformVisRef);
                            ImGui::Checkbox("Tri Indicator", &settings.showTriIndicatorOverlay);

                            auto Row = [](const char* label, const char* idSel, const char* idAll,
                                          bool& sel, bool& all) {
                                ImGui::TableNextRow();
                                ImGui::TableNextColumn(); ImGui::TextUnformatted(label);
                                ImGui::TableNextColumn(); ImGui::Checkbox(idSel, &sel);
                                ImGui::TableNextColumn(); ImGui::Checkbox(idAll, &all);
                            };
                            constexpr ImGuiTableFlags kFlags =
                                ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoBordersInBody;

                            ImGui::Separator();
                            ImGui::TextUnformatted("Gizmos");
                            if (ImGui::BeginTable("##SceneGizmos", 3, kFlags))
                            {
                                ImGui::TableSetupColumn("##label", ImGuiTableColumnFlags_WidthStretch);
                                ImGui::TableSetupColumn("Selected");
                                ImGui::TableSetupColumn("All");
                                ImGui::TableHeadersRow();
                                Row("Lights",  "##lightsSel",  "##lightsAll",  settings.lightsSelected,  settings.lightsAll);
                                Row("Cameras", "##camerasSel", "##camerasAll", settings.camerasSelected, settings.camerasAll);
                                Row("Bounds",  "##boundsSel",  "##boundsAll",  settings.boundsSelected,  settings.boundsAll);
                                Row("Bones",   "##bonesSel",   "##bonesAll",   settings.bonesSelected,   settings.bonesAll);
                                Row("Fog",     "##fogSel",     "##fogAll",     settings.fogSelected,     settings.fogAll);
                                Row("Wind",    "##windSel",    "##windAll",    settings.windSelected,    settings.windAll);
                                ImGui::EndTable();
                            }

                            ImGui::Separator();
                            ImGui::TextUnformatted("Physics");
                            if (ImGui::BeginTable("##ScenePhysicsGizmos", 3, kFlags))
                            {
                                ImGui::TableSetupColumn("##label", ImGuiTableColumnFlags_WidthStretch);
                                ImGui::TableSetupColumn("Selected");
                                ImGui::TableSetupColumn("All");
                                ImGui::TableHeadersRow();
                                Row("Colliders",      "##physShapesSel", "##physShapesAll",
                                    settings.physicsShapesSelected, settings.physicsShapesAll);
                                Row("AABBs",          "##physAABBsSel",  "##physAABBsAll",
                                    settings.physicsAABBsSelected,  settings.physicsAABBsAll);
                                Row("Center of Mass", "##physCoMSel",    "##physCoMAll",
                                    settings.physicsCoMSelected,    settings.physicsCoMAll);
                                ImGui::EndTable();
                            }

                            ImGui::PopFont();
                            ImGui::PopStyleVar();
                        }))
                    {
                        if (!gizState) {
                            // Toggling OFF: snapshot every flag so the next ON restores the exact set.
                            for (int i = 0; i < kGizmoToggleCount; ++i) { s_savedGizmo[i] = *toggles[i]; *toggles[i] = false; }
                            s_savedGizmoValid = true;
                        }
                        else if (s_savedGizmoValid) {
                            for (int i = 0; i < kGizmoToggleCount; ++i) *toggles[i] = s_savedGizmo[i];
                        }
                        else {
                            // no prior snapshot: enable a sensible default set
                            *xformVisRef = true;
                            settings.lightsSelected        = true;
                            settings.camerasSelected       = true;
                            settings.physicsShapesSelected = true;
                        }
                    }
                }

                ImGui::SameLine(0, gap);

                const char* overlayTip = m_ShowControlsOverlay ? "Hide controls overlay" : "Show controls overlay";
                UI::IconToggleButton("Overlay", ICON_KEYBOARD, overlayTip, &m_ShowControlsOverlay);
            }

            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));

            m_Viewport->BeginViewport();

            // Frame Debugger viewport overlay (Unity-style): when Frozen and
            // the panel is set to overlay in the Scene viewport, render the
            // selected pass's archived RT instead of the live LDR.
            const FrameDebuggerPanel::OverlaySource fdOverlay =
                [] {
                    auto* fd = Editor::GetPanel<FrameDebuggerPanel>();
                    if (fd && fd->ShouldOverlayInScene()) return fd->GetOverlaySource();
                    return FrameDebuggerPanel::OverlaySource{};
                }();

            if (fdOverlay.view != VK_NULL_HANDLE)
                m_Viewport->DrawSceneTextureRaw(fdOverlay.view, fdOverlay.sampler);
            else
                m_Viewport->DrawSceneTexture(m_RenderingSystem);

            // Handle gizmos
            m_Gizmo->DrawManipulator(
                m_EditorCamera.GetViewMatrix(),
                m_EditorCamera.GetProjectionMatrix(),
                m_Viewport->GetBounds(), m_Viewport->GetSize(),
                m_SelectedEntity, m_Context.get(),
                m_Viewport->IsHovered(), m_EditorCamera.IsFlying());

            // Debug overlays
            m_Overlays->DrawAll(m_Context, m_EditorCamera, m_SelectedEntity);

            // Play-mode viewport border tint (green=Playing, yellow=Paused)
            {
                const PlayState ps = PlayModeController::GetState();
                if (ps != PlayState::Editing) {
                    const ImU32 col = (ps == PlayState::Playing)
                        ? IM_COL32(80, 180, 100, 220)
                        : IM_COL32(220, 180, 80, 220);
                    ImGui::GetForegroundDrawList()->AddRect(
                        m_Viewport->GetBounds()[0],
                        m_Viewport->GetBounds()[1],
                        col, 0.0f, 0, 3.0f);
                }
            }

            // Plain LMB (no Alt, not over gizmo/icon): a click point-picks, a drag rubber-band-selects.
            // Resolved on RELEASE so a click and a drag are told apart.
            const bool altHeld = ImGui::IsKeyDown(ImGuiKey_LeftAlt) || ImGui::IsKeyDown(ImGuiKey_RightAlt);
            if (m_Viewport->IsHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)
                && !ImGuizmo::IsOver() && !m_Gizmo->WasIconClicked() && !altHeld)
            {
                m_MarqueePending = true;
                m_MarqueeActive  = false;
                m_MarqueeStart   = ImGui::GetMousePos();
            }

            if (m_MarqueePending && ImGui::IsMouseDown(ImGuiMouseButton_Left))
            {
                ImVec2 m = ImGui::GetMousePos();
                float dx = m.x - m_MarqueeStart.x; dx = dx < 0 ? -dx : dx;
                float dy = m.y - m_MarqueeStart.y; dy = dy < 0 ? -dy : dy;
                if (dx > 4.0f || dy > 4.0f) m_MarqueeActive = true;
            }

            if (m_MarqueePending && ImGui::IsMouseReleased(ImGuiMouseButton_Left))
            {
                if (m_MarqueeActive)
                {
                    SelectEntitiesInRect(m_MarqueeStart, ImGui::GetMousePos());
                }
                else
                {
                    // Click (no drag) -> point pick at the press position.
                    int px = (int)(m_MarqueeStart.x - m_Viewport->GetBounds()[0].x);
                    int py = (int)(m_MarqueeStart.y - m_Viewport->GetBounds()[0].y);
                    if (px >= 0 && px < m_Viewport->GetSize().x && py >= 0 && py < m_Viewport->GetSize().y)
                        if (auto* ps = SystemRegistry::GetSystem<PickingSystem>())
                            ps->RequestPick(px, py);
                }
                m_MarqueePending = false;
                m_MarqueeActive  = false;
            }

            // Rubber-band rect while dragging.
            if (m_MarqueeActive)
            {
                ImVec2 a = m_MarqueeStart, b = ImGui::GetMousePos();
                ImVec2 mn(std::min(a.x, b.x), std::min(a.y, b.y));
                ImVec2 mx(std::max(a.x, b.x), std::max(a.y, b.y));
                ImDrawList* dl = ImGui::GetWindowDrawList();
                dl->AddRectFilled(mn, mx, IM_COL32(80, 140, 255, 40));
                dl->AddRect(mn, mx, IM_COL32(80, 140, 255, 200));
            }

            // consume pick result: hierarchy-aware + multi-select
            auto* picker = SystemRegistry::GetSystem<PickingSystem>();
            if (!m_Gizmo->WasIconClicked() && picker && picker->HasResult())
            {
                entt::entity picked = picker->ConsumeResult();
                if (picked != entt::null && m_Context)
                {
                    Entity rawEntity(picked, m_Context.get());
                    if (rawEntity.IsValid())
                    {
                        bool ctrlHeld  = ImGui::IsKeyDown(ImGuiKey_LeftCtrl)  || ImGui::IsKeyDown(ImGuiKey_RightCtrl);
                        bool shiftHeld = ImGui::IsKeyDown(ImGuiKey_LeftShift) || ImGui::IsKeyDown(ImGuiKey_RightShift);

                        if (ctrlHeld)
                        {
                            // Ctrl+Click: toggle exact entity (no hierarchy traversal)
                            EditorSelection::ToggleEntity(rawEntity);
                        }
                        else if (shiftHeld)
                        {
                            // Shift+Click: add exact entity to selection
                            EditorSelection::AddEntity(rawEntity);
                        }
                        else
                        {
                            // First click on a model selects its root; once any part of that model is
                            // already selected, subsequent clicks select the exact part under the cursor.
                            Entity root = rawEntity.GetRoot();
                            Entity primary = EditorSelection::GetSelectedEntity();
                            bool sameModel = primary.IsValid() && primary.GetRoot() == root;
                            EditorSelection::SelectEntity(sameModel ? rawEntity : root);
                        }
                    }
                    else
                    {
                        EditorSelection::ClearSelection();
                    }
                }
                else
                {
                    EditorSelection::ClearSelection();
                }
            }

            // deferred icon selection always wins over pick results
            if (m_Gizmo->WasIconClicked() && m_Gizmo->IconEntity() != entt::null && m_Context)
            {
                // Discard any stale pick result
                if (picker && picker->HasResult())
                    picker->ConsumeResult();

                Entity e(m_Gizmo->IconEntity(), m_Context.get());
                EditorSelection::SelectEntity(e);
            }

            // Camera Control
            ImGui::SetNavCursorVisible(!m_EditorCamera.IsFlying());
            if (m_Viewport->IsHovered()) {
                // F = frame selected, Shift+F = lock/track selected
                if (ImGui::IsKeyPressed(ImGuiKey_F)) {
                    bool shiftHeld = ImGui::IsKeyDown(ImGuiKey_LeftShift) || ImGui::IsKeyDown(ImGuiKey_RightShift);
                    if (m_SelectedEntity && m_SelectedEntity.IsValid()) {
                        if (shiftHeld) {
                            m_EditorCamera.SetLockedEntity(m_SelectedEntity);
                        } else {
                            m_EditorCamera.ClearLockedEntity();
                            Vec3 newFocus = m_SelectedEntity.GetComponent<Transform>().Position;
                            m_EditorCamera.SetFocalPoint(newFocus);
                        }
                    }
                }

                m_EditorCamera.OnUpdate(Time::DeltaTime());
            }

            // Controls overlay (bottom-left)
            if (m_ShowControlsOverlay && m_Viewport->IsHovered() && m_Viewport->GetSize().x > 0 && m_Viewport->GetSize().y > 0)
            {
                std::vector<std::string> pressedKeys;
                
                // Mouse
                if (ImGui::IsMouseDown(ImGuiMouseButton_Left))   pressedKeys.push_back("LMB");
                if (ImGui::IsMouseDown(ImGuiMouseButton_Right))  pressedKeys.push_back("RMB");
                if (ImGui::IsMouseDown(ImGuiMouseButton_Middle)) pressedKeys.push_back("MMB");

                // Modifiers
                if (ImGui::GetIO().KeyCtrl)  pressedKeys.push_back("Ctrl");
                if (ImGui::GetIO().KeyShift) pressedKeys.push_back("Shift");
                if (ImGui::GetIO().KeyAlt)   pressedKeys.push_back("Alt");
                
                // Other keys
                for (int keyInt = ImGuiKey_NamedKey_BEGIN; keyInt < ImGuiKey_NamedKey_END; keyInt++)
                {
                    ImGuiKey key = static_cast<ImGuiKey>(keyInt);
                    // Skip modifiers and mouse buttons as we added them grouped above
                    if (key == ImGuiKey_LeftCtrl || key == ImGuiKey_RightCtrl || key == ImGuiKey_ModCtrl || key == ImGuiKey_ReservedForModCtrl ||
                        key == ImGuiKey_LeftShift || key == ImGuiKey_RightShift || key == ImGuiKey_ModShift || key == ImGuiKey_ReservedForModShift ||
                        key == ImGuiKey_LeftAlt || key == ImGuiKey_RightAlt || key == ImGuiKey_ModAlt || key == ImGuiKey_ReservedForModAlt ||
                        key == ImGuiKey_LeftSuper || key == ImGuiKey_RightSuper || key == ImGuiKey_ModSuper || key == ImGuiKey_ReservedForModSuper ||
                        key == ImGuiKey_MouseLeft || key == ImGuiKey_MouseRight || key == ImGuiKey_MouseMiddle ||
                        key == ImGuiKey_MouseX1 || key == ImGuiKey_MouseX2)
                        continue;

                    if (ImGui::IsKeyDown(key))
                    {
                        const char* name = ImGui::GetKeyName(key);
                        if (name) pressedKeys.push_back(name);
                    }
                }

                if (!pressedKeys.empty())
                {
                    ImGui::PushFont(Editor::GetMainFont());
                    std::string displayText = "";
                    for (size_t i = 0; i < pressedKeys.size(); i++) {
                        displayText += pressedKeys[i];
                        if (i < pressedKeys.size() - 1) displayText += " + ";
                    }

                    float pad = 12.0f;
                    ImVec2 textSize = ImGui::CalcTextSize(displayText.c_str());
                    float boxW = textSize.x + pad * 2.0f;
                    float boxH = textSize.y + pad * 2.0f;

                    float vpBottom = m_Viewport->GetBounds()[1].y;
                    float vpLeft   = m_Viewport->GetBounds()[0].x;

                    ImVec2 boxMin = { vpLeft + pad, vpBottom - boxH - pad };
                    ImVec2 boxMax = { vpLeft + pad + boxW, vpBottom - pad };

                    ImDrawList* dl = ImGui::GetWindowDrawList();
                    dl->AddRectFilled(boxMin, boxMax, IM_COL32(0.2, 0.2, 0.2, 128), 8.0f);
                    dl->AddText({ boxMin.x + pad, boxMin.y + pad }, IM_COL32(255, 255, 255, 255), displayText.c_str());
                    ImGui::PopFont();
                }
            }

            // Tri indicator overlay (top-right)
            if (Editor::GetSettings().showTriIndicatorOverlay
                && m_Viewport->GetSize().x > 0 && m_Viewport->GetSize().y > 0)
            {
                u32 triCount = m_RenderingSystem->GetTriangleCount();
                char triText[64];
                if (triCount >= 1000000)
                    snprintf(triText, sizeof(triText), ICON_SHAPES "  %.2fM tris", triCount / 1000000.0f);
                else if (triCount >= 1000)
                    snprintf(triText, sizeof(triText), ICON_SHAPES "  %.1fk tris", triCount / 1000.0f);
                else
                    snprintf(triText, sizeof(triText), ICON_SHAPES "  %u tris", triCount);

                const float triPad = 12.0f;
                ImVec2 triSize = ImGui::CalcTextSize(triText);
                float boxW = triSize.x + triPad * 2.0f;
                float boxH = triSize.y + triPad * 2.0f;
                float vpTop   = m_Viewport->GetBounds()[0].y;
                float vpRight = m_Viewport->GetBounds()[1].x;
                ImVec2 boxMin = { vpRight - boxW - triPad, vpTop + triPad };
                ImVec2 boxMax = { vpRight - triPad,        vpTop + triPad + boxH };

                ImDrawList* dl = ImGui::GetWindowDrawList();
                dl->AddRectFilled(boxMin, boxMax, IM_COL32(0.2, 0.2, 0.2, 128), 8.0f);
                dl->AddText({ boxMin.x + triPad, boxMin.y + triPad }, IM_COL32(255, 255, 255, 255), triText);
            }

            ImGui::PopStyleVar();
        }
        ImGui::End();
        ImGui::PopFont();
    }

    void ScenePanel::DrawDebugModePicker()
    {
        // Descriptor table: adding a mode is one row here (+ its engine viz), not a hand-written
        // radio + a manual active-range test. `gate` greys modes whose feature is off.
        enum class Gate : u8 { None, VolFog, RestirGi, RestirDi, Gtao, Reflections };
        struct Desc { const char* cat; const char* label; ShadeMode mode; Gate gate; };
        static constexpr Desc kModes[] = {
            { "Surface",         "Normals",          ShadeMode::Normals,             Gate::None },
            { "Surface",         "Metallic",         ShadeMode::Metallic,            Gate::None },
            { "Surface",         "Occlusion",        ShadeMode::Occlusion,           Gate::None },
            { "Surface",         "Emission",         ShadeMode::Emission,            Gate::None },
            { "Surface",         "Entity ID",        ShadeMode::EntityID,            Gate::None },
            { "G-buffer (Slim)", "Slim Normal",      ShadeMode::SlimNormal,          Gate::None },
            { "G-buffer (Slim)", "Slim Roughness",   ShadeMode::SlimRoughness,       Gate::None },
            { "G-buffer (Slim)", "Slim Motion",      ShadeMode::SlimMotion,          Gate::None },
            { "G-buffer (Slim)", "Slim Material ID", ShadeMode::SlimMaterialID,      Gate::None },
            { "Lighting",        "Cluster Density",  ShadeMode::ClustersDensity,     Gate::None },
            { "Lighting",        "Shadow Cascades",  ShadeMode::ShadowCascades,      Gate::None },
            { "Lighting",        "Ambient Occlusion",ShadeMode::AmbientOcclusion,    Gate::Gtao },
            { "GI / RT",         "GI Reservoir",     ShadeMode::RestirGiReservoir,   Gate::RestirGi },
            { "GI / RT",         "GI Raw",           ShadeMode::GiRaw,               Gate::RestirGi },
            { "GI / RT",         "DI Raw",           ShadeMode::DiRaw,               Gate::RestirDi },
            { "GI / RT",         "RT Reflection Raw",ShadeMode::RtReflectionRaw,     Gate::Reflections },
            { "Volumetrics",     "Vol Density",      ShadeMode::VolumetricDensity,   Gate::VolFog },
            { "Volumetrics",     "Vol In-Scatter",   ShadeMode::VolumetricInScatter, Gate::VolFog },
        };

        auto& settings      = Editor::GetSettings();
        const bool ptActive = m_RenderingSystem->GetRenderMode() == RenderMode::PathTrace;
        const bool fogOn    = settings.enableVolumetricFog;
        const bool giOn     = m_RenderingSystem->GetRestirGiSettings().enabled;
        const bool diOn     = m_RenderingSystem->GetRestirSettings().enabled;
        const bool gtaoOn   = m_RenderingSystem->GetPostProcessSettings().gtao.enabled;
        const bool reflOn   = m_RenderingSystem->GetReflectionsSettings().enabled;

        UI::CategoryItem items[IM_ARRAYSIZE(kModes)];
        for (int i = 0; i < IM_ARRAYSIZE(kModes); ++i)
        {
            const Desc& d = kModes[i];
            const char* reason = nullptr;
            bool enabled = true;
            if (ptActive)                                    { enabled = false; reason = "Disabled in Path-Trace mode"; }
            else if (d.gate == Gate::VolFog      && !fogOn)  { enabled = false; reason = "Enable Volumetric Fog"; }
            else if (d.gate == Gate::RestirGi    && !giOn)   { enabled = false; reason = "Enable ReSTIR GI"; }
            else if (d.gate == Gate::RestirDi    && !diOn)   { enabled = false; reason = "Enable ReSTIR DI"; }
            else if (d.gate == Gate::Gtao        && !gtaoOn) { enabled = false; reason = "Enable Ambient Occlusion (GTAO)"; }
            else if (d.gate == Gate::Reflections && !reflOn) { enabled = false; reason = "Enable RT Reflections"; }
            items[i] = { d.cat, d.label, (int)d.mode, enabled, reason };
        }

        int current = (int)m_RenderingSystem->GetShadeMode();
        if (UI::CategoryList("DebugModes", items, IM_ARRAYSIZE(kModes), &current,
                             m_DebugModeFilter, sizeof(m_DebugModeFilter)))
        {
            settings.lastDebugMode = (u8)current;
            m_RenderingSystem->SetShadeMode((ShadeMode)current);
        }
    }
}
