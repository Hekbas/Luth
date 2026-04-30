#include "lepch.h"
#include "luthien/panels/ScenePanel.h"
#include "luthien/panels/RenderPanel.h"
#include "luthien/EditorSelection.h"
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

    void ScenePanel::OnRender()
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

        if (ImGui::Begin(scene.c_str(), nullptr, ImGuiWindowFlags_NoScrollbar)) {
            // Toolbar â Left | Mid | Right
            {
                const float toolbarWidth = ImGui::GetContentRegionAvail().x;
                const float framePad = ImGui::GetStyle().FramePadding.x;
                const float itemSpacing = ImGui::GetStyle().ItemSpacing.x;
                const float btnSize = ImGui::GetFrameHeight();
                const float sepWidth = 2.0f + itemSpacing * 2.0f;

                // --- Accent color for active tool button ---
                ImVec4 activeCol = ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive);
                ImVec4 normalCol = ImGui::GetStyleColorVec4(ImGuiCol_Button);

                auto ToolButton = [&](const char* icon, const char* id, const char* tooltip, int gizmoOp) {
                    bool isActive = (m_Gizmo->GetOperation() == gizmoOp);
                    if (isActive) {
                        ImGui::PushStyleColor(ImGuiCol_Button, activeCol);
                        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, activeCol);
                    }
                    std::string label = std::string(icon) + id;
                    if (ImGui::Button(label.c_str(), { btnSize, btnSize }))
                        m_Gizmo->SetOperation(gizmoOp);
                    if (isActive)
                        ImGui::PushStyleColor(ImGuiCol_Border, activeCol); // pop below
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s", tooltip);
                    if (isActive)
                        ImGui::PopStyleColor(3); // Button, ButtonHovered, Border
                };

                // ── Left: gizmo tools ──
                ImGui::AlignTextToFramePadding();
                ToolButton(ICON_FA_CROSSHAIRS, "##Select", "Select (Q)", -1);
                ImGui::SameLine(0, 2.0f);
                ToolButton(ICON_FA_ARROWS_UP_DOWN_LEFT_RIGHT, "##Translate", "Translate (W)", ImGuizmo::OPERATION::TRANSLATE);
                ImGui::SameLine(0, 2.0f);
                ToolButton(ICON_FA_ROTATE, "##Rotate", "Rotate (E)", ImGuizmo::OPERATION::ROTATE);
                ImGui::SameLine(0, 2.0f);
                ToolButton(ICON_FA_EXPAND, "##Scale", "Scale (R)", ImGuizmo::OPERATION::SCALE);

                // ── Transport controls ──
                ImGui::SameLine(0, 4.0f);
                ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
                ImGui::SameLine(0, 4.0f);

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

                // Play / Resume — same button, tooltip shifts based on state
                const bool canPlay   = (playState == PlayState::Editing);
                const bool canResume = (playState == PlayState::Paused);
                if (TransportBtn(ICON_FA_PLAY, "##Play",
                                 canResume ? "Resume" : "Play",
                                 canPlay || canResume)) {
                    if (canResume) PlayModeController::Resume();
                    else           PlayModeController::EnterPlay();
                }
                ImGui::SameLine(0, 2.0f);

                if (TransportBtn(ICON_FA_PAUSE, "##Pause", "Pause", playState == PlayState::Playing))
                    PlayModeController::Pause();
                ImGui::SameLine(0, 2.0f);

                if (TransportBtn(ICON_FA_STOP, "##Stop", "Stop", playState != PlayState::Editing))
                    PlayModeController::Stop();
                ImGui::SameLine(0, 2.0f);

                if (TransportBtn(ICON_FA_FORWARD_STEP, "##Step", "Step one frame",
                                 playState == PlayState::Paused))
                    PlayModeController::RequestStep();

                ImGui::SameLine();

                // ── Center: stats / view ──
                // Pre-calculate mid section width
                u32 triCount = m_RenderingSystem->GetTriangleCount();
                char triText[64];
                if (triCount >= 1000000)
                    snprintf(triText, sizeof(triText), ICON_FA_SHAPES "  %.2fM tris", triCount / 1000000.0f);
                else if (triCount >= 1000)
                    snprintf(triText, sizeof(triText), ICON_FA_SHAPES "  %.1fk tris", triCount / 1000.0f);
                else
                    snprintf(triText, sizeof(triText), ICON_FA_SHAPES "  %u tris", triCount);

                float triTextW = ImGui::CalcTextSize(triText).x;
                float eyeIconW = ImGui::CalcTextSize(ICON_FA_EYE).x;
                float comboW = 90.0f;
                float sunIconW = ImGui::CalcTextSize(ICON_FA_SUN).x;
                float midWidth = triTextW + sepWidth + eyeIconW + itemSpacing + comboW + sunIconW;

                float midStart = (toolbarWidth - midWidth) * 0.5f;
                float cursorX = ImGui::GetCursorPosX();
                if (midStart > cursorX)
                    ImGui::SameLine(midStart);
                else
                    ImGui::SameLine();

                ImGui::AlignTextToFramePadding();
                ImGui::Text("%s", triText);

                ImGui::SameLine();
                ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
                ImGui::SameLine();

                ImGui::Text(ICON_FA_EYE);
                ImGui::SameLine();
                static const char* shadeModeNames[] = { "Lit", "Unlit", "Wireframe", "Normals", "EntityID" };
                int currentMode = static_cast<int>(m_RenderingSystem->GetShadeMode());
                ImGui::SetNextItemWidth(comboW);
                if (ImGui::Combo("##ShadeMode", &currentMode, shadeModeNames, IM_ARRAYSIZE(shadeModeNames)))
                    m_RenderingSystem->SetShadeMode(static_cast<ShadeMode>(currentMode));
                
                ImGui::SameLine();
                
                // Environment
                ButtonDropdown(ICON_FA_SUN, "##Environment", [this]() {
                    ImGui::PushFont(Editor::GetMainFont());
                    
                    auto& settings = Editor::GetSettings();
                    ImGui::Text("HDR: %s", settings.skyboxPath.c_str());
                    if (ImGui::Button("Browse HDR..."))
                    {
                        auto result = FileDialog::OpenFile("HDR Environment\0*.hdr;*.exr\0All Files\0*.*\0");
                        if (result.has_value())
                        {
                            fs::path absPath = result.value();
                            fs::path assetsPath = FileSystem::AssetsPath();
                            auto rel = fs::relative(absPath, assetsPath);
                            if (!rel.empty() && rel.string().find("..") == std::string::npos)
                                settings.skyboxPath = rel.generic_string();
                            else
                                settings.skyboxPath = absPath.generic_string();

                            m_RenderingSystem->ReloadSkybox(absPath);
                            Editor::MarkDirty();
                        }
                    }

                    ImGui::PopFont();
                });
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Environment");

                // ── Right: camera & overlay ──
                float rightWidth = btnSize*3 + sepWidth + itemSpacing*2;
                float rightStart = toolbarWidth - rightWidth + ImGui::GetStyle().WindowPadding.x;
                ImGui::SameLine(rightStart);

                // Camera settings
                ButtonDropdown(ICON_FA_CAMERA, "##CamPopup", [this]() {
                    ImGui::PushFont(Editor::GetMainFont());

                    // Fly speed
                    if (UI::BeginProperties("CamSpeedProps")) {
                        UI::Property("Fly Speed", m_EditorCamera.GetFlySpeedRef(), 0.1f, 0.1f, 200.0f);
                        UI::EndProperties();
                    }

                    ImGui::Separator();

                    // Camera settings
                    ImGui::Text("Camera");
                    ImGui::Spacing();
                    
                    if (UI::BeginProperties("CameraProps")) {
                        if (UI::Property("FOV", m_EditorCamera.GetFOVRef(), 1.0f, 30.0f, 120.0f))
                            m_EditorCamera.SetFOV(m_EditorCamera.GetFOV());
                        UI::Property("Near Clip", m_EditorCamera.GetNearClipRef(), 0.01f, 0.01f, 10.0f);
                        UI::Property("Far Clip", m_EditorCamera.GetFarClipRef(), 1.0f, 100.0f, 50000.0f);
                        UI::EndProperties();
                    }

                    ImGui::Separator();

                    ImGui::Text("Controls");
                    ImGui::Spacing();
                    
                    if (UI::BeginProperties("ControlsProps")) {
                        UI::Property("Rotation Speed", m_EditorCamera.GetRotationSpeedRef(), 10.0f, 1000.0f, 50000.0f);
                        UI::Property("Pan Speed", m_EditorCamera.GetPanSpeedRef(), 1.0f, 10.0f, 1000.0f);
                        UI::Property("Zoom Speed", m_EditorCamera.GetZoomSpeedRef(), 1.0f, 10.0f, 500.0f);
                        UI::Property("Shift Multiplier", m_EditorCamera.GetShiftMultiplierRef(), 0.1f, 1.0f, 10.0f);
                        UI::EndProperties();
                    }
                    
                    ImGui::PopFont();
                });
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Camera settings");

                ImGui::SameLine();
                
                // Gizmo visibility dropdown
                ButtonDropdown(ICON_FA_EYE, "##GizmoVis", [this]() {
                    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(2, 2));
                    ImGui::PushFont(Editor::GetMainFont());
                    ImGui::Checkbox("Transform Gizmo", m_Gizmo->GetTransformGizmoVisibleRef());
                    ImGui::Checkbox("Grid", &Editor::GetSettings().showGrid);
                    ImGui::Checkbox("Bone Debug", &Editor::GetSettings().showBoneDebug);
                    ImGui::Checkbox("Light Gizmos", &Editor::GetSettings().showLightGizmos);
                    ImGui::Checkbox("Camera Gizmos", &Editor::GetSettings().showCameraGizmos);
                    ImGui::Checkbox("AABB Gizmos", &Editor::GetSettings().showAABBGizmos);
                    ImGui::PopFont();
                    ImGui::PopStyleVar();
                });
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Gizmo visibility");
                
                ImGui::SameLine();
                
                // Controls overlay toggle
                ImGui::SameLine();
                ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
                ImGui::SameLine();
                if (ImGui::Button(m_ShowControlsOverlay ? ICON_FA_KEYBOARD "##OverlayOn" : ICON_FA_KEYBOARD "##OverlayOff", { btnSize, btnSize }))
                    m_ShowControlsOverlay = !m_ShowControlsOverlay;
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip(m_ShowControlsOverlay ? "Hide controls overlay" : "Show controls overlay");
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

            ImGui::PopStyleVar();
        }
        ImGui::End();
        ImGui::PopFont();
    }
}
