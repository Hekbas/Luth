#include "lepch.h"
#include "luthien/panels/ScenePanel.h"
#include "luthien/panels/RenderPanel.h"
#include "luthien/EditorSelection.h"
#include "luthien/commands/Commands.h"
#include "luthien/CommandHistory.h"
#include "luthien/EditorSettings.h"
#include "luthien/EditorColors.h"
#include "luthien/viewport/ViewportRenderer.h"
#include "luth/platform/FileDialog.h"
#include "luth/resources/FileSystem.h"
#include "luth/scene/Components.h"
#include "luth/scene/systems/PickingSystem.h"
#include "luth/scene/systems/SystemRegistry.h"
#include "luth/renderer/Renderer.h"
#include "luth/events/RenderEvent.h"
#include "luthien/widgets/ImGuiUtils.h"
#include "luthien/widgets/Icons.h"
#include "luthien/widgets/Widgets.h"
#include "luth/renderer/resources/Model.h"
#include "luth/resources/AssetManager.h"
#include <ImGuizmo.h>

namespace Luth
{
    using namespace Component;

    ScenePanel::ScenePanel(RenderingSystem* renderingSystem)
        : m_RenderingSystem(renderingSystem)
        , m_Viewport(std::make_unique<ViewportRenderer>())
        , m_Gizmo(std::make_unique<GizmoController>())
    {
        m_EditorCamera = EditorCamera(70.0f, 1.77f, 0.1f, 10000.0f);
        m_Gizmo->SetOperation(ImGuizmo::OPERATION::TRANSLATE);

        EventBus::Subscribe<RenderResizeEvent>(BusType::MainThread, [this](Event& e) {
            HandleRenderResize(e);
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
        m_Gizmo->ResetFrameState();

        // Sync selection (primary = last-added for gizmos/camera)
        m_SelectedEntity = EditorSelection::GetSelectedEntity();
        auto& oc = EditorColors::SelectionOutline;
        m_RenderingSystem->SetOutlineColor(oc.x, oc.y, oc.z, oc.w);
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
            m_Viewport->DrawSceneTexture(m_RenderingSystem);

            // Handle gizmos
            m_Gizmo->DrawManipulator(
                m_EditorCamera.GetViewMatrix(),
                m_EditorCamera.GetProjectionMatrix(),
                m_Viewport->GetBounds(), m_Viewport->GetSize(),
                m_SelectedEntity, m_Context.get(),
                m_Viewport->IsFocused(), m_EditorCamera.IsFlying());

            // Debug overlays
            DrawBoneDebugOverlay();
            DrawLightGizmos();
            DrawCameraGizmos();
            DrawAABBGizmos();

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

    /*void ScenePanel::SetViewportCamera(const std::shared_ptr<Camera>& camera) {
        m_EditorCamera = camera;
        if (m_EditorCamera) {
            m_EditorCamera->SetViewportSize(m_Viewport->GetSize().x, m_Viewport->GetSize().y);
        }
    }*/

    void ScenePanel::DrawBoneDebugOverlay()
    {
        if (!Editor::GetSettings().showBoneDebug) return;

        Entity selectedEntity = EditorSelection::GetSelectedEntity();
        if (!selectedEntity || !selectedEntity.IsValid()) return;

        // Find the entity that owns Animation â selected entity or its parent
        Entity animEntity = selectedEntity;
        if (!animEntity.HasComponent<Animation>() && animEntity.HasParent()) {
            Entity parent = animEntity.GetParent();
            if (parent && parent.HasComponent<Animation>())
                animEntity = parent;
        }
        if (!animEntity.HasComponent<Animation>()) return;
        if (!animEntity.HasComponent<WorldTransform>()) return;

        auto& anim = animEntity.GetComponent<Animation>();
        auto& worldTransform = animEntity.GetComponent<WorldTransform>();

        if (anim.GlobalBoneTransforms.empty()) return;

        auto model = AssetManager::GetAsset<Model>(anim.ModelUUID);
        if (!model) return;
        const auto& skeleton = model->GetSkeleton();
        if (skeleton.IsEmpty()) return;

        ImDrawList* drawList = ImGui::GetWindowDrawList();
        drawList->PushClipRect(m_Viewport->GetBounds()[0], m_Viewport->GetBounds()[1], true);

        u32 boneCount = skeleton.BoneCount();
        u32 transformCount = (u32)anim.GlobalBoneTransforms.size();
        u32 count = std::min(boneCount, transformCount);

        std::vector<ImVec2> screenPositions(count);
        std::vector<bool> visible(count, false);

        for (u32 i = 0; i < count; i++) {
            Vec3 boneLocalPos = Vec3(anim.GlobalBoneTransforms[i][3]);
            Vec3 boneWorldPos = Vec3(worldTransform.Matrix * Vec4(boneLocalPos, 1.0f));
            screenPositions[i] = ProjectToScreen(boneWorldPos);
            visible[i] = IsInViewport(screenPositions[i]);
        }

        ImU32 lineColor  = IM_COL32(0, 255, 128, 200);
        ImU32 jointColor = IM_COL32(255, 255, 0, 255);

        for (u32 i = 0; i < count; i++) {
            i32 parentIdx = skeleton.Bones[i].ParentIndex;
            if (parentIdx >= 0 && parentIdx < (i32)count) {
                if (visible[i] || visible[parentIdx]) {
                    drawList->AddLine(screenPositions[parentIdx], screenPositions[i],
                                      lineColor, 2.0f);
                }
            }
            if (visible[i]) {
                drawList->AddCircleFilled(screenPositions[i], 3.0f, jointColor);
            }
        }

        drawList->PopClipRect();
    }

    // ── Shared gizmo helpers ──

    ImVec2 ScenePanel::ProjectToScreen(const Vec3& worldPos) const
    {
        Vec4 clipPos = m_EditorCamera.GetViewProjection() * Vec4(worldPos, 1.0f);
        if (clipPos.w <= 0.001f) return { -1.0f, -1.0f };
        Vec3 ndc = Vec3(clipPos) / clipPos.w;
        float screenX = m_Viewport->GetBounds()[0].x + (ndc.x * 0.5f + 0.5f) * m_Viewport->GetSize().x;
        float screenY = m_Viewport->GetBounds()[0].y + (-ndc.y * 0.5f + 0.5f) * m_Viewport->GetSize().y;
        return { screenX, screenY };
    }

    bool ScenePanel::IsInViewport(const ImVec2& p) const
    {
        return p.x >= m_Viewport->GetBounds()[0].x && p.x <= m_Viewport->GetBounds()[1].x
            && p.y >= m_Viewport->GetBounds()[0].y && p.y <= m_Viewport->GetBounds()[1].y;
    }

    ImU32 ScenePanel::LightColorToImU32(const Vec3& color, float alpha) const
    {
        return IM_COL32(
            (u8)(Math::Clamp(color.r, 0.0f, 1.0f) * 255.0f),
            (u8)(Math::Clamp(color.g, 0.0f, 1.0f) * 255.0f),
            (u8)(Math::Clamp(color.b, 0.0f, 1.0f) * 255.0f),
            (u8)(alpha * 255.0f));
    }

    bool ScenePanel::ClipLineToNearPlane(Vec3& a, Vec3& b) const
    {
        Mat4 vp = m_EditorCamera.GetViewProjection();
        // Compute clip-space w for each endpoint (w = row3 dot (x,y,z,1))
        float wa = vp[0][3] * a.x + vp[1][3] * a.y + vp[2][3] * a.z + vp[3][3];
        float wb = vp[0][3] * b.x + vp[1][3] * b.y + vp[2][3] * b.z + vp[3][3];
        constexpr float eps = 0.01f;
        if (wa < eps && wb < eps) return false; // both behind camera
        if (wa < eps) { float t = (eps - wa) / (wb - wa); a = Math::Mix(a, b, t); }
        if (wb < eps) { float t = (eps - wb) / (wa - wb); b = Math::Mix(b, a, t); }
        return true;
    }

    void ScenePanel::DrawClippedLine(ImDrawList* drawList, const Vec3& worldA, const Vec3& worldB,
                                     ImU32 color, float thickness)
    {
        Vec3 a = worldA, b = worldB;
        if (!ClipLineToNearPlane(a, b)) return;
        drawList->AddLine(ProjectToScreen(a), ProjectToScreen(b), color, thickness);
    }

    // ── Light Gizmos ──

    void ScenePanel::DrawLightGizmos()
    {
        if (!Editor::GetSettings().showLightGizmos) return;
        if (!m_Context) return;

        auto& registry = m_Context->Registry();
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        drawList->PushClipRect(m_Viewport->GetBounds()[0], m_Viewport->GetBounds()[1], true);

        // --- Directional Lights ---
        {
            auto view = registry.view<WorldTransform, DirectionalLight>();
            for (auto entity : view) {
                auto& wt = view.get<WorldTransform>(entity);
                auto& dl = view.get<DirectionalLight>(entity);

                Vec3 pos = Vec3(wt.Matrix[3]);
                Vec3 dir = Math::Normalize(-Vec3(wt.Matrix[2]));
                ImU32 color = LightColorToImU32(dl.Color);

                // Icon (only if in front of camera)
                ImVec2 screenPos = ProjectToScreen(pos);
                if (screenPos.x >= 0.0f)
                    m_Gizmo->DrawGizmoIcon(drawList, screenPos, ICON_FA_SUN, color, entity, m_Viewport->IsHovered(), m_SelectedEntity && m_SelectedEntity.IsValid());

                // Direction arrow â only when selected
                Entity e(entity, m_Context.get());
                if (!EditorSelection::IsSelected(e)) continue;

                // Constant screen-size: compute pixel-per-unit at this depth
                Vec3 startPos = pos + dir * 1.5f; // offset to avoid icon overlap
                ImVec2 startScreen = ProjectToScreen(startPos);
                ImVec2 unitScreen  = ProjectToScreen(startPos + dir);
                float pxPerUnit = sqrtf((unitScreen.x - startScreen.x) * (unitScreen.x - startScreen.x)
                                      + (unitScreen.y - startScreen.y) * (unitScreen.y - startScreen.y));
                if (pxPerUnit < 0.001f) continue;
                constexpr float desiredPx = 80.0f;
                float worldLen = Math::Clamp(desiredPx / pxPerUnit, 0.5f, 50.0f);

                Vec3 endWorld = startPos + dir * worldLen;
                ImVec2 endScreen = ProjectToScreen(endWorld);

                DrawClippedLine(drawList, startPos, endWorld, color, 2.0f);

                // Arrowhead (in screen space)
                ImVec2 dir2D = { endScreen.x - startScreen.x, endScreen.y - startScreen.y };
                float len = sqrtf(dir2D.x * dir2D.x + dir2D.y * dir2D.y);
                if (len > 1.0f) {
                    ImVec2 norm = { dir2D.x / len, dir2D.y / len };
                    ImVec2 perp = { -norm.y, norm.x };
                    constexpr float arrowSize = 10.0f;
                    ImVec2 a1 = { endScreen.x - norm.x * arrowSize + perp.x * arrowSize * 0.5f,
                                  endScreen.y - norm.y * arrowSize + perp.y * arrowSize * 0.5f };
                    ImVec2 a2 = { endScreen.x - norm.x * arrowSize - perp.x * arrowSize * 0.5f,
                                  endScreen.y - norm.y * arrowSize - perp.y * arrowSize * 0.5f };
                    drawList->AddTriangleFilled(endScreen, a1, a2, color);
                }
            }
        }

        // --- Point Lights ---
        {
            auto view = registry.view<WorldTransform, PointLight>();
            for (auto entity : view) {
                auto& wt = view.get<WorldTransform>(entity);
                auto& pl = view.get<PointLight>(entity);

                Vec3 center = Vec3(wt.Matrix[3]);
                ImU32 iconColor = LightColorToImU32(pl.Color);

                // Icon (only if in front of camera)
                ImVec2 screenCenter = ProjectToScreen(center);
                if (screenCenter.x >= 0.0f)
                    m_Gizmo->DrawGizmoIcon(drawList, screenCenter, ICON_FA_LIGHTBULB, iconColor, entity, m_Viewport->IsHovered(), m_SelectedEntity && m_SelectedEntity.IsValid());

                // Range circles â only when selected
                Entity e(entity, m_Context.get());
                if (!EditorSelection::IsSelected(e)) continue;

                float radius = pl.Range;
                ImU32 color = LightColorToImU32(pl.Color, 0.6f);

                constexpr int segments = 32;
                constexpr float twoPi = Math::TwoPi<float>;

                for (int plane = 0; plane < 3; plane++) {
                    Vec3 prevWorld;
                    for (int i = 0; i <= segments; i++) {
                        float angle = (float)i / (float)segments * twoPi;
                        float c = cosf(angle) * radius;
                        float s = sinf(angle) * radius;
                        Vec3 offset;
                        if (plane == 0)      offset = Vec3(c, s, 0.0f);
                        else if (plane == 1) offset = Vec3(c, 0.0f, s);
                        else                 offset = Vec3(0.0f, c, s);

                        Vec3 worldPt = center + offset;
                        if (i > 0)
                            DrawClippedLine(drawList, prevWorld, worldPt, color, 1.5f);
                        prevWorld = worldPt;
                    }
                }
            }
        }

        drawList->PopClipRect();
    }

    // ── Camera Gizmos ──

    void ScenePanel::DrawCameraGizmos()
    {
        if (!Editor::GetSettings().showCameraGizmos) return;
        if (!m_Context) return;

        auto& registry = m_Context->Registry();
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        drawList->PushClipRect(m_Viewport->GetBounds()[0], m_Viewport->GetBounds()[1], true);

        auto view = registry.view<WorldTransform, Camera>();
        for (auto entity : view) {
            auto& wt  = view.get<WorldTransform>(entity);
            auto& cam = view.get<Camera>(entity);

            Vec3 pos = Vec3(wt.Matrix[3]);

            // Icon (only if in front of camera)
            ImVec2 screenPos = ProjectToScreen(pos);
            if (screenPos.x >= 0.0f)
                m_Gizmo->DrawGizmoIcon(drawList, screenPos, ICON_FA_VIDEO, EditorColors::GizmoCamera, entity, m_Viewport->IsHovered(), m_SelectedEntity && m_SelectedEntity.IsValid());

            // Compute frustum corners in camera local space (looking along -Z)
            float visualFar = Math::Min(cam.FarClip, 1000.0f);
            Vec3 nearCorners[4], farCorners[4];

            if (cam.Projection == Camera::ProjectionType::Perspective) {
                float fovRad = Math::Radians(cam.VerticalFOV);
                float nearH = tanf(fovRad * 0.5f) * cam.NearClip;
                float nearW = nearH * cam.AspectRatio;
                float farH  = tanf(fovRad * 0.5f) * visualFar;
                float farW  = farH * cam.AspectRatio;

                nearCorners[0] = Vec3(-nearW,  nearH, -cam.NearClip);
                nearCorners[1] = Vec3( nearW,  nearH, -cam.NearClip);
                nearCorners[2] = Vec3( nearW, -nearH, -cam.NearClip);
                nearCorners[3] = Vec3(-nearW, -nearH, -cam.NearClip);

                farCorners[0] = Vec3(-farW,  farH, -visualFar);
                farCorners[1] = Vec3( farW,  farH, -visualFar);
                farCorners[2] = Vec3( farW, -farH, -visualFar);
                farCorners[3] = Vec3(-farW, -farH, -visualFar);
            }
            else {
                float halfH = cam.OrthographicSize * 0.5f;
                float halfW = halfH * cam.AspectRatio;

                nearCorners[0] = Vec3(-halfW,  halfH, -cam.OrthographicNear);
                nearCorners[1] = Vec3( halfW,  halfH, -cam.OrthographicNear);
                nearCorners[2] = Vec3( halfW, -halfH, -cam.OrthographicNear);
                nearCorners[3] = Vec3(-halfW, -halfH, -cam.OrthographicNear);

                float orthoFar = Math::Min(cam.OrthographicFar, 50.0f);
                farCorners[0] = Vec3(-halfW,  halfH, -orthoFar);
                farCorners[1] = Vec3( halfW,  halfH, -orthoFar);
                farCorners[2] = Vec3( halfW, -halfH, -orthoFar);
                farCorners[3] = Vec3(-halfW, -halfH, -orthoFar);
            }

            // Transform to world space
            Vec3 nearWorld[4], farWorld[4];
            for (int i = 0; i < 4; i++) {
                nearWorld[i] = Vec3(wt.Matrix * Vec4(nearCorners[i], 1.0f));
                farWorld[i]  = Vec3(wt.Matrix * Vec4(farCorners[i], 1.0f));
            }

            ImU32 color = EditorColors::GizmoCamera;

            // Near plane quad
            for (int i = 0; i < 4; i++)
                DrawClippedLine(drawList, nearWorld[i], nearWorld[(i + 1) % 4], color, 1.5f);
            // Far plane quad
            for (int i = 0; i < 4; i++)
                DrawClippedLine(drawList, farWorld[i], farWorld[(i + 1) % 4], color, 1.5f);
            // Connecting edges
            for (int i = 0; i < 4; i++)
                DrawClippedLine(drawList, nearWorld[i], farWorld[i], color, 1.0f);
        }

        drawList->PopClipRect();
    }

    // ── AABB Gizmos ──

    void ScenePanel::DrawAABBGizmos()
    {
        if (!Editor::GetSettings().showAABBGizmos) return;
        if (!m_Context) return;

        auto& registry = m_Context->Registry();
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        drawList->PushClipRect(m_Viewport->GetBounds()[0], m_Viewport->GetBounds()[1], true);

        constexpr int edges[12][2] = {
            {0,1},{1,2},{2,3},{3,0},
            {4,5},{5,6},{6,7},{7,4},
            {0,4},{1,5},{2,6},{3,7}
        };

        auto view = registry.view<WorldTransform, MeshRenderer>();
        for (auto entity : view) {
            auto& wt = view.get<WorldTransform>(entity);
            auto& mr = view.get<MeshRenderer>(entity);

            // Check for animated AABB first
            AABB aabb;
            bool worldSpace = false;
            if (registry.all_of<Animation>(entity)) {
                auto& anim = registry.get<Animation>(entity);
                if (anim.AnimatedAABB.IsValid()) {
                    aabb = anim.AnimatedAABB;
                    worldSpace = true;
                }
            }

            // Fall back to bind-pose AABB from model
            if (!worldSpace) {
                auto model = AssetManager::GetAsset<Model>(mr.ModelUUID);
                if (!model) continue;
                auto& meshes = model->GetMeshesData();
                if (mr.MeshIndex >= meshes.size()) continue;
                aabb = meshes[mr.MeshIndex].BindPoseAABB;
                if (!aabb.IsValid()) continue;
            }

            // Compute 8 world-space corners
            Vec3 mn = aabb.Min, mx = aabb.Max;
            Vec3 localCorners[8] = {
                {mn.x, mn.y, mn.z}, {mx.x, mn.y, mn.z},
                {mx.x, mx.y, mn.z}, {mn.x, mx.y, mn.z},
                {mn.x, mn.y, mx.z}, {mx.x, mn.y, mx.z},
                {mx.x, mx.y, mx.z}, {mn.x, mx.y, mx.z},
            };

            Vec3 worldCorners[8];
            for (int i = 0; i < 8; i++)
                worldCorners[i] = worldSpace ? localCorners[i] : Vec3(wt.Matrix * Vec4(localCorners[i], 1.0f));

            Entity e(entity, m_Context.get());
            ImU32 color = EditorSelection::IsSelected(e)
                ? EditorColors::GizmoAABBSelected
                : EditorColors::GizmoAABB;

            for (auto& [a, b] : edges)
                DrawClippedLine(drawList, worldCorners[a], worldCorners[b], color, 1.0f);
        }

        drawList->PopClipRect();
    }

    void ScenePanel::HandleRenderResize(Event& e)
    {
        if (e.IsInCategory(EventCategoryRender)) {
            auto& resizeEvent = static_cast<RenderResizeEvent&>(e);
            m_RenderingSystem->Resize(resizeEvent.GetWidth(), resizeEvent.GetHeight());
            m_EditorCamera.SetViewportSize((float)resizeEvent.GetWidth(), (float)resizeEvent.GetHeight());
            m_Viewport->SetSize(resizeEvent.GetWidth(), resizeEvent.GetHeight());
            e.m_Handled = true;
            LH_CORE_TRACE("Resized Viewport {0}x{1}", resizeEvent.GetWidth(), resizeEvent.GetHeight());
        }
    }

}
