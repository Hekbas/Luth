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
#include "luth/scene/systems/PickingSystem.h"
#include "luth/scene/systems/SystemRegistry.h"
#include "luth/renderer/Renderer.h"
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

        LH_CORE_INFO("Created Scene panel");
    }

    ScenePanel::~ScenePanel() = default;

    void ScenePanel::OnInit()
    {
        m_SelectedEntity = EditorSelection::GetSelectedEntity();
    }

    void ScenePanel::OnGather(EditorSnapshotBuilder& builder)
    {
        // Most viewport work is unavoidably ImGui-driven (ImGuizmo, ImGui::Image of the
        // rendertarget, drag-drop). Camera matrices feed RenderingSystem via
        // EditorViewportState in App::Run; capturing them here is future polish.
        auto* snap = builder.Add<SceneViewportSnapshot>();
        snap->selectionVersion = EditorSelection::GetVersion();
    }

    void ScenePanel::OnDraw(const EditorSnapshot& /*snapshot*/)
    {
        LH_PROFILE_FUNCTION();
        m_Gizmo->ResetFrameState();

        // Sync selection (primary = last-added for gizmos/camera)
        m_SelectedEntity = EditorSelection::GetSelectedEntity();
        // Outline color now flows through EditorSettings → EditorViewportState → CameraParams
        // (see luth/core/App.cpp). Visibility-only toggles still go through the direct setter.
        m_RenderingSystem->SetGridVisible(Editor::GetSettings().showGrid);

        ImGui::PushFont(Editor::GetFASolid());
        std::string scene = ICON_FA_GAMEPAD + std::string("  Scene");

        if (BeginWindow(scene.c_str(), ImGuiWindowFlags_NoScrollbar)) {
            // Toolbar - Left (gizmos + grid) | Transport | Right (render + dropdowns)
            {
                auto& settings = Editor::GetSettings();
                const ImGuiStyle& style  = ImGui::GetStyle();
                const float toolbarWidth = ImGui::GetContentRegionAvail().x;
                const float btnSize      = ImGui::GetFrameHeight();
                // invariant: matches widgets/ButtonGroup IconBtnSize() — icon-toggle
                // buttons add 2*(FramePadding.x - FramePadding.y) to width so the
                // glyph isn't pinched horizontally at default style {6,4}.
                const float iconBtnW     = btnSize + 2.0f * std::max(0.0f, style.FramePadding.x - style.FramePadding.y);
                const float chevW        = btnSize * 0.75f;     // matches SplitToggleButton chevron half
                const float splitW       = iconBtnW + chevW;
                const float gap          = 2.0f;
                const float groupGap     = 8.0f;
                const float wideGap      = 14.0f;               // gizmo|grid + debug|camera separation

                // Pre-compute block widths so transport/right blocks land deterministically.
                // Transport + render-mode buttons are plain ImGui::Button({btnSize,btnSize}),
                // not icon-toggles — keep btnSize. Splits + Overlay use iconBtnW.
                // invariant: SameLine(absX) is window-relative, NOT content-relative —
                // it ignores WindowPadding.x, so the absX values must include it.
                const float windowPadX     = style.WindowPadding.x;
                const float transportW     = (4 * btnSize) + (3 * gap);
                const float renderModeW    = (4 * btnSize) + (3 * gap);
                const float rightW         = renderModeW + groupGap + splitW + wideGap + splitW + gap + splitW + gap + iconBtnW;
                const float transportStart = windowPadX + (toolbarWidth - transportW) * 0.5f;
                const float rightStart     = windowPadX + (toolbarWidth - rightW);

                // LEFT: gizmo tools + grid split
                ImGui::AlignTextToFramePadding();
                static const char* kGizmoIcons[]    = { ICON_FA_ARROW_POINTER, ICON_FA_ARROWS_UP_DOWN_LEFT_RIGHT, ICON_FA_ROTATE, ICON_FA_EXPAND };
                static const char* kGizmoTooltips[] = { "Select (Q)", "Translate (W)", "Rotate (E)", "Scale (R)" };
                static constexpr int kGizmoOps[]    = { -1, ImGuizmo::OPERATION::TRANSLATE, ImGuizmo::OPERATION::ROTATE, ImGuizmo::OPERATION::SCALE };
                int gizmoIdx = 0;
                const int currentOp = m_Gizmo->GetOperation();
                for (int i = 0; i < IM_ARRAYSIZE(kGizmoOps); ++i) if (kGizmoOps[i] == currentOp) { gizmoIdx = i; break; }
                if (UI::IconToggleGroup("GizmoTools", kGizmoIcons, kGizmoTooltips, IM_ARRAYSIZE(kGizmoOps), &gizmoIdx))
                    m_Gizmo->SetOperation(kGizmoOps[gizmoIdx]);

                ImGui::SameLine(0, wideGap);

                UI::SplitToggleButton("Grid", ICON_FA_TABLE_CELLS, "Grid",
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
                    bool clicked = ImGui::Button(label.c_str(), { btnSize, btnSize });
                    if (ImGui::IsItemHovered() && enabled)
                        ImGui::SetTooltip("%s", tooltip);
                    if (!enabled) ImGui::EndDisabled();
                    return clicked;
                };

                const bool canPlay   = (playState == PlayState::Editing);
                const bool canResume = (playState == PlayState::Paused);
                if (TransportBtn(ICON_FA_PLAY, "##Play",
                                 canResume ? "Resume" : "Play",
                                 canPlay || canResume)) {
                    if (canResume) PlayModeController::Resume();
                    else           PlayModeController::EnterPlay();
                }
                ImGui::SameLine(0, gap);
                if (TransportBtn(ICON_FA_PAUSE, "##Pause", "Pause", playState == PlayState::Playing))
                    PlayModeController::Pause();
                ImGui::SameLine(0, gap);
                if (TransportBtn(ICON_FA_STOP, "##Stop", "Stop", playState != PlayState::Editing))
                    PlayModeController::Stop();
                ImGui::SameLine(0, gap);
                if (TransportBtn(ICON_FA_FORWARD_STEP, "##Step", "Step one frame",
                                 playState == PlayState::Paused))
                    PlayModeController::RequestStep();

                // RIGHT: render modes + debug split + camera split + gizmo-vis split + overlay toggle
                ImGui::SameLine(rightStart);

                struct RenderModeBtn { const char* icon; const char* tip; int mode; };
                static const RenderModeBtn kRenderModes[] = {
                    { ICON_FA_GLOBE,              "Wireframe",                                 (int)ShadeMode::Wireframe },
                    { ICON_FA_GLOBE,              "Shaded Wireframe (engine support pending)", -1 },
                    { ICON_FA_CIRCLE,             "Unlit",                                     (int)ShadeMode::Unlit },
                    { ICON_FA_CIRCLE_HALF_STROKE, "Lit",                                       (int)ShadeMode::Lit },
                };
                const int curMode = (int)m_RenderingSystem->GetShadeMode();
                const ImVec4 activeCol = ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive);
                for (int i = 0; i < IM_ARRAYSIZE(kRenderModes); ++i) {
                    const bool active  = (curMode == kRenderModes[i].mode);
                    const bool enabled = (kRenderModes[i].mode != -1);
                    if (active) {
                        ImGui::PushStyleColor(ImGuiCol_Button, activeCol);
                        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, activeCol);
                    }
                    if (!enabled) ImGui::BeginDisabled();
                    ImGui::PushID(i);
                    if (ImGui::Button(kRenderModes[i].icon, { btnSize, btnSize }) && enabled)
                        m_RenderingSystem->SetShadeMode((ShadeMode)kRenderModes[i].mode);
                    ImGui::PopID();
                    if (!enabled) ImGui::EndDisabled();
                    if (active) ImGui::PopStyleColor(2);
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", kRenderModes[i].tip);
                    if (i + 1 < IM_ARRAYSIZE(kRenderModes)) ImGui::SameLine(0, gap);
                }

                ImGui::SameLine(0, groupGap);

                // Debug split — icon toggles current<->lastDebugMode; chevron picks the mode.
                {
                    const bool dbgActive = (curMode == (int)ShadeMode::Normals)
                                        || (curMode == (int)ShadeMode::EntityID)
                                        || (curMode == (int)ShadeMode::Emission)
                                        || (curMode >= (int)ShadeMode::SlimNormal && curMode <= (int)ShadeMode::SlimMaterialID)
                                        || (curMode == (int)ShadeMode::ClustersDensity)
                                        || (curMode == (int)ShadeMode::VolumetricDensity)
                                        || (curMode == (int)ShadeMode::VolumetricInScatter)
                                        || (curMode == (int)ShadeMode::RestirGiReservoir);
                    bool dbgState = dbgActive;
                    if (UI::SplitToggleButton("Debug", ICON_FA_BUG, "Debug Render Modes", &dbgState,
                        [&]() {
                            ImGui::PushFont(Editor::GetMainFont());
                            if (ImGui::RadioButton("Normals", curMode == (int)ShadeMode::Normals)) {
                                settings.lastDebugMode = (u8)ShadeMode::Normals;
                                m_RenderingSystem->SetShadeMode(ShadeMode::Normals);
                            }
                            if (ImGui::RadioButton("EntityID", curMode == (int)ShadeMode::EntityID)) {
                                settings.lastDebugMode = (u8)ShadeMode::EntityID;
                                m_RenderingSystem->SetShadeMode(ShadeMode::EntityID);
                            }
                            if (ImGui::RadioButton("Emission", curMode == (int)ShadeMode::Emission)) {
                                settings.lastDebugMode = (u8)ShadeMode::Emission;
                                m_RenderingSystem->SetShadeMode(ShadeMode::Emission);
                            }
                            ImGui::Separator();
                            ImGui::TextDisabled("Slim G-buffer");
                            if (ImGui::RadioButton("Slim Normal",       curMode == (int)ShadeMode::SlimNormal)) {
                                settings.lastDebugMode = (u8)ShadeMode::SlimNormal;
                                m_RenderingSystem->SetShadeMode(ShadeMode::SlimNormal);
                            }
                            if (ImGui::RadioButton("Slim Roughness",    curMode == (int)ShadeMode::SlimRoughness)) {
                                settings.lastDebugMode = (u8)ShadeMode::SlimRoughness;
                                m_RenderingSystem->SetShadeMode(ShadeMode::SlimRoughness);
                            }
                            if (ImGui::RadioButton("Slim Motion",       curMode == (int)ShadeMode::SlimMotion)) {
                                settings.lastDebugMode = (u8)ShadeMode::SlimMotion;
                                m_RenderingSystem->SetShadeMode(ShadeMode::SlimMotion);
                            }
                            if (ImGui::RadioButton("Slim Material ID",  curMode == (int)ShadeMode::SlimMaterialID)) {
                                settings.lastDebugMode = (u8)ShadeMode::SlimMaterialID;
                                m_RenderingSystem->SetShadeMode(ShadeMode::SlimMaterialID);
                            }
                            ImGui::Separator();
                            ImGui::TextDisabled("Forward+ Clusters");
                            if (ImGui::RadioButton("Cluster Density",   curMode == (int)ShadeMode::ClustersDensity)) {
                                settings.lastDebugMode = (u8)ShadeMode::ClustersDensity;
                                m_RenderingSystem->SetShadeMode(ShadeMode::ClustersDensity);
                            }
                            ImGui::Separator();
                            ImGui::TextDisabled("Volumetric Fog");
                            if (ImGui::RadioButton("Vol Density",       curMode == (int)ShadeMode::VolumetricDensity)) {
                                settings.lastDebugMode = (u8)ShadeMode::VolumetricDensity;
                                m_RenderingSystem->SetShadeMode(ShadeMode::VolumetricDensity);
                            }
                            if (ImGui::RadioButton("Vol In-Scatter",    curMode == (int)ShadeMode::VolumetricInScatter)) {
                                settings.lastDebugMode = (u8)ShadeMode::VolumetricInScatter;
                                m_RenderingSystem->SetShadeMode(ShadeMode::VolumetricInScatter);
                            }
                            ImGui::Separator();
                            ImGui::TextDisabled("ReSTIR GI");
                            if (ImGui::RadioButton("GI Reservoir (M/age)", curMode == (int)ShadeMode::RestirGiReservoir)) {
                                settings.lastDebugMode = (u8)ShadeMode::RestirGiReservoir;
                                m_RenderingSystem->SetShadeMode(ShadeMode::RestirGiReservoir);
                            }
                            ImGui::PopFont();
                        }))
                    {
                        m_RenderingSystem->SetShadeMode(dbgState
                            ? (ShadeMode)settings.lastDebugMode
                            : ShadeMode::Lit);
                    }
                }

                ImGui::SameLine(0, wideGap);

                // Camera split (chevron-only).
                UI::SplitToggleButton("Camera", ICON_FA_CAMERA, "Camera Settings", nullptr,
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
                    });

                ImGui::SameLine(0, gap);

                // Gizmo visibility split — icon toggles all gizmos on/off; chevron lists
                // per-gizmo flags + the tri-indicator overlay (Grid lives in its own split now).
                {
                    // Saved-flags snapshot covers both the legacy gizmo bools and the new physics
                    // Selected/All pairs so the icon toggle restores everything at once.
                    static struct {
                        bool transform; bool bone; bool light; bool camera; bool aabb;
                        bool physShapesSel, physShapesAll;
                        bool physAABBsSel,  physAABBsAll;
                        bool physCoMSel,    physCoMAll;
                        bool valid;
                    } s_savedGizmoFlags{};
                    bool* xformVisRef = m_Gizmo->GetTransformGizmoVisibleRef();
                    bool gizState = (*xformVisRef) || settings.showBoneDebug
                                  || settings.showLightGizmos || settings.showCameraGizmos
                                  || settings.showAABBGizmos
                                  || settings.physicsShapesSelected || settings.physicsShapesAll
                                  || settings.physicsAABBsSelected  || settings.physicsAABBsAll
                                  || settings.physicsCoMSelected    || settings.physicsCoMAll;
                    if (UI::SplitToggleButton("GizmoVis", ICON_FA_EYE, "Gizmos", &gizState,
                        [this, &settings, xformVisRef]() {
                            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(2, 2));
                            ImGui::PushFont(Editor::GetMainFont());
                            ImGui::Checkbox("Transform Gizmo", xformVisRef);
                            ImGui::Checkbox("Bone Debug",      &settings.showBoneDebug);
                            ImGui::Checkbox("Light Gizmos",    &settings.showLightGizmos);
                            ImGui::Checkbox("Camera Gizmos",   &settings.showCameraGizmos);
                            ImGui::Checkbox("AABB Gizmos",     &settings.showAABBGizmos);
                            ImGui::Checkbox("Tri Indicator",   &settings.showTriIndicatorOverlay);

                            ImGui::Separator();
                            ImGui::TextUnformatted("Physics");
                            if (ImGui::BeginTable("##ScenePhysicsGizmos", 3,
                                ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoBordersInBody))
                            {
                                ImGui::TableSetupColumn("##label", ImGuiTableColumnFlags_WidthStretch);
                                ImGui::TableSetupColumn("Selected");
                                ImGui::TableSetupColumn("All");
                                ImGui::TableHeadersRow();

                                auto Row = [](const char* label, const char* idSel, const char* idAll,
                                              bool& sel, bool& all) {
                                    ImGui::TableNextRow();
                                    ImGui::TableNextColumn(); ImGui::TextUnformatted(label);
                                    ImGui::TableNextColumn(); ImGui::Checkbox(idSel, &sel);
                                    ImGui::TableNextColumn(); ImGui::Checkbox(idAll, &all);
                                };
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
                            // Toggling OFF: snapshot per-flag state so the next ON can restore it.
                            s_savedGizmoFlags = {
                                *xformVisRef, settings.showBoneDebug,
                                settings.showLightGizmos, settings.showCameraGizmos,
                                settings.showAABBGizmos,
                                settings.physicsShapesSelected, settings.physicsShapesAll,
                                settings.physicsAABBsSelected,  settings.physicsAABBsAll,
                                settings.physicsCoMSelected,    settings.physicsCoMAll,
                                true
                            };
                            *xformVisRef = false;
                            settings.showBoneDebug = false;
                            settings.showLightGizmos = false;
                            settings.showCameraGizmos = false;
                            settings.showAABBGizmos = false;
                            settings.physicsShapesSelected = false;
                            settings.physicsShapesAll      = false;
                            settings.physicsAABBsSelected  = false;
                            settings.physicsAABBsAll       = false;
                            settings.physicsCoMSelected    = false;
                            settings.physicsCoMAll         = false;
                        }
                        else if (s_savedGizmoFlags.valid) {
                            *xformVisRef = s_savedGizmoFlags.transform;
                            settings.showBoneDebug   = s_savedGizmoFlags.bone;
                            settings.showLightGizmos = s_savedGizmoFlags.light;
                            settings.showCameraGizmos= s_savedGizmoFlags.camera;
                            settings.showAABBGizmos  = s_savedGizmoFlags.aabb;
                            settings.physicsShapesSelected = s_savedGizmoFlags.physShapesSel;
                            settings.physicsShapesAll      = s_savedGizmoFlags.physShapesAll;
                            settings.physicsAABBsSelected  = s_savedGizmoFlags.physAABBsSel;
                            settings.physicsAABBsAll       = s_savedGizmoFlags.physAABBsAll;
                            settings.physicsCoMSelected    = s_savedGizmoFlags.physCoMSel;
                            settings.physicsCoMAll         = s_savedGizmoFlags.physCoMAll;
                        }
                        else {
                            *xformVisRef = true;
                            settings.showLightGizmos = true;
                            settings.showCameraGizmos = true;
                            settings.physicsShapesSelected = true;
                        }
                    }
                }

                ImGui::SameLine(0, gap);

                const char* overlayTip = m_ShowControlsOverlay ? "Hide controls overlay" : "Show controls overlay";
                UI::IconToggleButton("Overlay", ICON_FA_KEYBOARD, overlayTip, &m_ShowControlsOverlay);
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
                m_Viewport->IsFocused(), m_EditorCamera.IsFlying());

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

            // Mouse picking â LMB click in viewport (not on gizmo or icon)
            if (m_Viewport->IsHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGuizmo::IsOver()
                && !m_Gizmo->WasIconClicked()
                && !ImGui::IsKeyDown(ImGuiKey_LeftAlt) && !ImGui::IsKeyDown(ImGuiKey_RightAlt))
            {
                auto [mx, my] = ImGui::GetMousePos();
                int px = (int)(mx - m_Viewport->GetBounds()[0].x);
                int py = (int)(my - m_Viewport->GetBounds()[0].y);
                // Ensure click is inside viewport
                if (px >= 0 && px < m_Viewport->GetSize().x && py >= 0 && py < m_Viewport->GetSize().y)
                    if (auto* ps = SystemRegistry::GetSystem<PickingSystem>())
                        ps->RequestPick(px, py);
            }

            // Consume pick result â hierarchy-aware + multi-select
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
                            // Plain click: hierarchy-aware selection
                            Entity root = rawEntity.GetRoot();
                            Entity lastRaw = EditorSelection::GetLastRawPick();

                            // Drill-down: if root is already selected and clicking same mesh again, select child
                            if (EditorSelection::IsSelected(root) && lastRaw == rawEntity && rawEntity != root)
                                EditorSelection::SelectEntity(rawEntity);
                            else
                                EditorSelection::SelectEntity(root);
                        }

                        EditorSelection::SetLastRawPick(rawEntity);
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

            // Deferred icon selection â always wins over pick results
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
                    snprintf(triText, sizeof(triText), ICON_FA_SHAPES "  %.2fM tris", triCount / 1000000.0f);
                else if (triCount >= 1000)
                    snprintf(triText, sizeof(triText), ICON_FA_SHAPES "  %.1fk tris", triCount / 1000.0f);
                else
                    snprintf(triText, sizeof(triText), ICON_FA_SHAPES "  %u tris", triCount);

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
}
