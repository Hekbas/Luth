#include "luthpch.h"
#include "luth/editor/panels/ScenePanel.h"
#include "luth/editor/panels/RenderPanel.h"
#include "luth/editor/EditorSelection.h"
#include "luth/editor/EditorSettings.h"
#include "luth/editor/EditorColors.h"
#include "luth/platform/FileDialog.h"
#include "luth/resources/FileSystem.h"
#include "luth/scene/Components.h"
#include "luth/renderer/Renderer.h"
#include "luth/platform/RenderEvent.h"
#include "luth/utils/ImGuiUtils.h"
#include "luth/utils/LuthIcons.h"
#include "luth/editor/UI.h"
#include "luth/renderer/Model.h"
#include "luth/resources/AssetManager.h"
#include "luth/renderer/backend/vulkan/VulkanTexture.h"
#include "luth/renderer/backend/vulkan/VulkanContext.h"
#include <backends/imgui_impl_vulkan.h>
#include <glm/gtc/type_ptr.hpp>
#include <ImGuizmo.h>

namespace Luth
{
    using namespace Component;

    ScenePanel::ScenePanel(std::shared_ptr<RenderingSystem> renderingSystem)
        : m_RenderingSystem(renderingSystem)
    {
        m_EditorCamera = EditorCamera(70.0f, 1.77f, 0.1f, 10000.0f);
        m_GizmoType = ImGuizmo::OPERATION::TRANSLATE;

        EventBus::Subscribe<RenderResizeEvent>(BusType::MainThread, [this](Event& e) {
            HandleRenderResize(e);
        });

        LH_CORE_INFO("Created Scene panel");
    }

    ScenePanel::~ScenePanel()
    {
        if (m_SceneDS) {
            ImGui_ImplVulkan_RemoveTexture(m_SceneDS);
            m_SceneDS = VK_NULL_HANDLE;
        }
        m_LastSceneTex.reset();
    }

    void ScenePanel::OnInit()
    {
        m_SelectedEntity = EditorSelection::GetSelectedEntity();
    }

    void ScenePanel::OnRender()
    {
        // Sync selection (primary = last-added for gizmos/camera)
        m_SelectedEntity = EditorSelection::GetSelectedEntity();
        auto& oc = EditorColors::SelectionOutline;
        m_RenderingSystem->SetOutlineColor(oc.x, oc.y, oc.z, oc.w);

        ImGui::PushFont(Editor::GetFASolid());
        std::string scene = ICON_FA_GAMEPAD + std::string("  Scene");

        if (ImGui::Begin(scene.c_str(), nullptr, ImGuiWindowFlags_NoScrollbar)) {
            // Toolbar — Left | Mid | Right
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
                    bool isActive = (m_GizmoType == gizmoOp);
                    if (isActive) {
                        ImGui::PushStyleColor(ImGuiCol_Button, activeCol);
                        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, activeCol);
                    }
                    std::string label = std::string(icon) + id;
                    if (ImGui::Button(label.c_str(), { btnSize, btnSize }))
                        m_GizmoType = gizmoOp;
                    if (isActive)
                        ImGui::PushStyleColor(ImGuiCol_Border, activeCol); // pop below
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s", tooltip);
                    if (isActive)
                        ImGui::PopStyleColor(3); // Button, ButtonHovered, Border
                };

                // ======= LEFT: Gizmo tools =======
                ImGui::AlignTextToFramePadding();
                ToolButton(ICON_FA_CROSSHAIRS, "##Select", "Select (Q)", -1);
                ImGui::SameLine(0, 2.0f);
                ToolButton(ICON_FA_ARROWS_UP_DOWN_LEFT_RIGHT, "##Translate", "Translate (W)", ImGuizmo::OPERATION::TRANSLATE);
                ImGui::SameLine(0, 2.0f);
                ToolButton(ICON_FA_ROTATE, "##Rotate", "Rotate (E)", ImGuizmo::OPERATION::ROTATE);
                ImGui::SameLine(0, 2.0f);
                ToolButton(ICON_FA_EXPAND, "##Scale", "Scale (R)", ImGuizmo::OPERATION::SCALE);

                ImGui::SameLine();

                // ======= MID: Stats / View (centered) =======
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

                // ======= RIGHT: Camera & Overlay (right-aligned) =======
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
                    ImGui::Checkbox("Transform Gizmo", &m_ShowTransformGizmo);
                    ImGui::Checkbox("Bone Debug", &Editor::GetSettings().showBoneDebug);
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

            // Viewport sizing — compare as integers to avoid an infinite resize
            // loop caused by float ↔ u32 truncation in the RenderResizeEvent path.
            const Vec2 avail = ToGlmVec2(ImGui::GetContentRegionAvail());
            const u32 newW = (u32)avail.x;
            const u32 newH = (u32)avail.y;
            const u32 curW = (u32)m_ViewportSize.x;
            const u32 curH = (u32)m_ViewportSize.y;
            if ((newW != curW || newH != curH) && newW > 0 && newH > 0) {
                m_ViewportSize = { (float)newW, (float)newH };

                // Update rendering system and camera
                EventBus::Enqueue<RenderResizeEvent>(BusType::MainThread, newW, newH);
            }

            // Update viewport bounds for gizmos & mouse picking
            ImVec2 cursorScreenPos = ImGui::GetCursorScreenPos();
            m_ViewportBounds[0] = cursorScreenPos;
            m_ViewportBounds[1] = { cursorScreenPos.x + m_ViewportSize.x, cursorScreenPos.y + m_ViewportSize.y };

            // Get final output from active rendering technique
            if (auto texture = m_RenderingSystem->GetSceneColor())
            {
                if (texture != m_LastSceneTex)
                {
                    if (m_SceneDS)
                    {
                        VkDescriptorSet oldSet = m_SceneDS;
                        VulkanContext::Get().PushDeletion([oldSet]() {
                            ImGui_ImplVulkan_RemoveTexture(oldSet);
                        });
                    }

                    auto vkTex = std::static_pointer_cast<VKTexture>(texture);
                    m_SceneDS = ImGui_ImplVulkan_AddTexture(vkTex->GetSampler(), vkTex->GetImageView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                    m_LastSceneTex = texture;
                }

                ImGui::Image((ImTextureID)m_SceneDS, ToImVec2(m_ViewportSize), { 0, 0 }, { 1, 1 });
            }
            else
            {
                ImGui::Text("No Scene Output");
            }

            // Interaction states
            m_IsFocused = ImGui::IsWindowFocused();
            m_IsHovered = ImGui::IsWindowHovered();

            // Handle gizmos
            DrawGizmos();

            // Bone debug overlay
            DrawBoneDebugOverlay();

            // Mouse picking — LMB click in viewport (not on gizmo)
            if (m_IsHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGuizmo::IsOver()
                && !ImGui::IsKeyDown(ImGuiKey_LeftAlt) && !ImGui::IsKeyDown(ImGuiKey_RightAlt))
            {
                auto [mx, my] = ImGui::GetMousePos();
                int px = (int)(mx - m_ViewportBounds[0].x);
                int py = (int)(my - m_ViewportBounds[0].y);
                // Ensure click is inside viewport
                if (px >= 0 && px < m_ViewportSize.x && py >= 0 && py < m_ViewportSize.y)
                    m_RenderingSystem->RequestPick(px, py);
            }

            // Consume pick result — hierarchy-aware + multi-select
            if (m_RenderingSystem->HasPickResult())
            {
                entt::entity picked = m_RenderingSystem->ConsumePickResult();
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

            // Camera Control
            ImGui::SetNavCursorVisible(!m_EditorCamera.IsFlying());
            if (m_IsHovered) {
                // F = frame selected, Shift+F = lock/track selected
                if (ImGui::IsKeyPressed(ImGuiKey_F)) {
                    bool shiftHeld = ImGui::IsKeyDown(ImGuiKey_LeftShift) || ImGui::IsKeyDown(ImGuiKey_RightShift);
                    if (m_SelectedEntity && m_SelectedEntity.IsValid()) {
                        if (shiftHeld) {
                            m_EditorCamera.SetLockedEntity(m_SelectedEntity);
                        } else {
                            m_EditorCamera.ClearLockedEntity();
                            glm::vec3 newFocus = m_SelectedEntity.GetComponent<Transform>().Position;
                            m_EditorCamera.SetFocalPoint(newFocus);
                        }
                    }
                }

                m_EditorCamera.OnUpdate(Time::DeltaTime());
            }

            // Controls overlay (bottom-left)
            if (m_ShowControlsOverlay && m_IsHovered && m_ViewportSize.x > 0 && m_ViewportSize.y > 0)
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

                    float vpBottom = m_ViewportBounds[1].y;
                    float vpLeft   = m_ViewportBounds[0].x;

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
            m_EditorCamera->SetViewportSize(m_ViewportSize.x, m_ViewportSize.y);
        }
    }*/

    void ScenePanel::DrawGizmos()
    {
        if (!m_SelectedEntity || !m_SelectedEntity.IsValid()) return;

        ImGuizmo::SetOrthographic(false);
        ImGuizmo::SetDrawlist();

        // Use the exact viewport bounds calculated during OnRender
        ImGuizmo::SetRect(m_ViewportBounds[0].x, m_ViewportBounds[0].y, m_ViewportSize.x, m_ViewportSize.y);

        // Camera
        const glm::mat4& view = m_EditorCamera.GetViewMatrix();
        const glm::mat4& proj = m_EditorCamera.GetProjectionMatrix();

        // Entity Transform
        auto& tc = m_SelectedEntity.GetComponent<Transform>();
        
        // Get World Matrix for Gizmo
        glm::mat4 worldMatrix = m_SelectedEntity.GetComponent<WorldTransform>().Matrix;

        // If we are in Local mode, we still need the world matrix for position, but we want to edit in local axes.
        // ImGuizmo handles this via the MODE parameter.
        
        // However, if we edit in World mode, we need to decompose the result back to Local.
        // If dirty, reconstruct world matrix on the fly to be responsive
        if (tc.IsDirty)
        {
             // We can't easily reconstruct world matrix here without parent info.
             // Rely on the System to have updated it, OR force a quick calc if needed.
             // For now, let's trust the system update loop which runs before Render.
        }

        // Only draw & interact with the manipulator when visible and a tool is active
        if (m_ShowTransformGizmo && m_GizmoType != -1)
        {
            // Snapping
            bool snap = ImGui::IsKeyDown(ImGuiKey_LeftCtrl);
            float snapValue = 0.5f; // Snap to 0.5m for translation/scale
            if (m_GizmoType == ImGuizmo::OPERATION::ROTATE)
                snapValue = 45.0f; // Snap to 45 degrees for rotation

            float snapValues[3] = { snapValue, snapValue, snapValue };

            ImGuizmo::Manipulate(glm::value_ptr(view), glm::value_ptr(proj),
                (ImGuizmo::OPERATION)m_GizmoType, ImGuizmo::LOCAL, glm::value_ptr(worldMatrix),
                nullptr, snap ? snapValues : nullptr);

            if (ImGuizmo::IsUsing())
            {
                // Convert back to Local Space
                glm::mat4 localMatrix = worldMatrix;
                if (m_SelectedEntity.HasParent())
                {
                    Entity parent = m_SelectedEntity.GetParent();
                    glm::mat4 parentWorld = parent.GetComponent<WorldTransform>().Matrix;
                    localMatrix = glm::inverse(parentWorld) * worldMatrix;
                }

                float translation[3], rotation[3], scale[3];
                ImGuizmo::DecomposeMatrixToComponents(glm::value_ptr(localMatrix), translation, rotation, scale);

                tc.Position = glm::make_vec3(translation);
                tc.Rotation = glm::make_vec3(rotation);
                tc.Scale = glm::make_vec3(scale);
                tc.IsDirty = true;
            }
        }

        // Gizmo Shortcuts
        if (m_IsFocused && !ImGuizmo::IsUsing() && !m_EditorCamera.IsFlying())
        {
            if (ImGui::IsKeyPressed(ImGuiKey_Q))
                m_GizmoType = -1;
            if (ImGui::IsKeyPressed(ImGuiKey_W))
                m_GizmoType = ImGuizmo::OPERATION::TRANSLATE;
            if (ImGui::IsKeyPressed(ImGuiKey_E))
                m_GizmoType = ImGuizmo::OPERATION::ROTATE;
            if (ImGui::IsKeyPressed(ImGuiKey_R))
                m_GizmoType = ImGuizmo::OPERATION::SCALE;
        }
    }

    void ScenePanel::DrawBoneDebugOverlay()
    {
        if (!Editor::GetSettings().showBoneDebug) return;

        Entity selectedEntity = EditorSelection::GetSelectedEntity();
        if (!selectedEntity || !selectedEntity.IsValid()) return;

        // Find the entity that owns Animation — selected entity or its parent
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

        // VP matrix from editor camera
        Mat4 viewProj = m_EditorCamera.GetViewProjection();

        // Viewport screen rect
        ImVec2 vpMin = m_ViewportBounds[0];
        ImVec2 vpMax = m_ViewportBounds[1];

        ImDrawList* drawList = ImGui::GetWindowDrawList();
        drawList->PushClipRect(vpMin, vpMax, true);

        u32 boneCount = skeleton.BoneCount();
        u32 transformCount = (u32)anim.GlobalBoneTransforms.size();
        u32 count = std::min(boneCount, transformCount);

        // Project bone world positions to screen coords
        auto ProjectToScreen = [&](Vec3 worldPos) -> ImVec2 {
            Vec4 clipPos = viewProj * Vec4(worldPos, 1.0f);
            if (clipPos.w <= 0.001f) return { -1.0f, -1.0f };
            Vec3 ndc = Vec3(clipPos) / clipPos.w;
            float screenX = vpMin.x + (ndc.x * 0.5f + 0.5f) * m_ViewportSize.x;
            float screenY = vpMin.y + (-ndc.y * 0.5f + 0.5f) * m_ViewportSize.y;
            return { screenX, screenY };
        };

        std::vector<ImVec2> screenPositions(count);
        std::vector<bool> visible(count, false);

        for (u32 i = 0; i < count; i++) {
            Vec3 boneLocalPos = Vec3(anim.GlobalBoneTransforms[i][3]);
            Vec3 boneWorldPos = Vec3(worldTransform.Matrix * Vec4(boneLocalPos, 1.0f));
            screenPositions[i] = ProjectToScreen(boneWorldPos);
            visible[i] = (screenPositions[i].x >= vpMin.x && screenPositions[i].x <= vpMax.x
                       && screenPositions[i].y >= vpMin.y && screenPositions[i].y <= vpMax.y);
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

    void ScenePanel::HandleRenderResize(Event& e)
    {
        if (e.IsInCategory(EventCategoryRender)) {
            auto& resizeEvent = static_cast<RenderResizeEvent&>(e);
            m_RenderingSystem->Resize(resizeEvent.GetWidth(), resizeEvent.GetHeight());
            m_EditorCamera.SetViewportSize((float)resizeEvent.GetWidth(), (float)resizeEvent.GetHeight());
            m_ViewportSize = { (float)resizeEvent.GetWidth(), (float)resizeEvent.GetHeight() };
            e.m_Handled = true;
            LH_CORE_TRACE("Resized Viewport {0}x{1}", resizeEvent.GetWidth(), resizeEvent.GetHeight());
        }
    }


    // Editor Camera
    // ======================================
    EditorCamera::EditorCamera(float fov, float aspectRatio, float nearClip, float farClip)
        : m_FOV(fov), m_AspectRatio(aspectRatio), m_NearClip(nearClip), m_FarClip(farClip)
    {
        glm::vec3 direction = glm::normalize(m_FocalPoint - m_Position);
        m_Distance = glm::length(m_FocalPoint - m_Position);

        m_Yaw = glm::degrees(atan2(direction.z, direction.x));
        m_Pitch = glm::degrees(asin(direction.y));

        UpdateProjection();
        UpdateView();
    }

    void EditorCamera::OnUpdate(float dt) {
        auto [x, y] = ImGui::GetMousePos();
        glm::vec2 currentMousePos(x, y);
        glm::vec2 delta = (currentMousePos - m_LastMousePosition) * 0.002f;
        m_LastMousePosition = currentMousePos;

        // Input state
        bool rmb     = ImGui::IsMouseDown(ImGuiMouseButton_Right);
        bool mmb     = ImGui::IsMouseDown(ImGuiMouseButton_Middle);
        bool lmb     = ImGui::IsMouseDown(ImGuiMouseButton_Left);
        bool altHeld = ImGui::IsKeyDown(ImGuiKey_LeftAlt) || ImGui::IsKeyDown(ImGuiKey_RightAlt);
        bool shiftHeld = ImGui::IsKeyDown(ImGuiKey_LeftShift) || ImGui::IsKeyDown(ImGuiKey_RightShift);

        m_IsFlying = rmb && !altHeld;
        bool updated = false;

        // --- Flythrough mode (RMB without Alt) ---
        if (m_IsFlying)
        {
            // Mouse look
            m_Yaw   += delta.x * m_RotationSpeed * dt;
            m_Pitch -= delta.y * m_RotationSpeed * dt;
            m_Pitch  = glm::clamp(m_Pitch, -89.0f, 89.0f);

            // WASD + QE movement
            glm::vec3 forward = GetForwardDirection();
            glm::vec3 right   = GetRightDirection();
            glm::vec3 worldUp = glm::vec3(0.0f, 1.0f, 0.0f);

            float speed = m_FlySpeed * (shiftHeld ? m_ShiftMultiplier : 1.0f) * dt;
            glm::vec3 movement(0.0f);

            if (ImGui::IsKeyDown(ImGuiKey_W)) movement += forward;
            if (ImGui::IsKeyDown(ImGuiKey_S)) movement -= forward;
            if (ImGui::IsKeyDown(ImGuiKey_D)) movement += right;
            if (ImGui::IsKeyDown(ImGuiKey_A)) movement -= right;
            if (ImGui::IsKeyDown(ImGuiKey_E)) movement += worldUp;
            if (ImGui::IsKeyDown(ImGuiKey_Q)) movement -= worldUp;

            if (glm::length(movement) > 0.0001f)
                m_Position += glm::normalize(movement) * speed;

            // Scroll adjusts base fly speed while in flythrough
            float scrollDelta = ImGui::GetIO().MouseWheel;
            if (scrollDelta != 0.0f) {
                m_FlySpeed += scrollDelta * 0.5f;
                m_FlySpeed = glm::clamp(m_FlySpeed, 0.1f, 200.0f);
            }

            // Keep focal point in sync so orbit works after releasing RMB
            m_FocalPoint = m_Position + GetForwardDirection() * m_Distance;
            updated = true;
        }
        // --- Orbit mode (Alt + LMB) ---
        else if (altHeld && lmb)
        {
            m_Yaw   += delta.x * m_RotationSpeed * dt;
            m_Pitch -= delta.y * m_RotationSpeed * dt;
            m_Pitch  = glm::clamp(m_Pitch, -89.0f, 89.0f);
            m_Position = CalculatePosition();
            updated = true;
        }
        // --- Smooth zoom (Alt + RMB drag) ---
        else if (altHeld && rmb)
        {
            float zoomAmount = (delta.x + delta.y) * m_ZoomSpeed * m_Distance * dt;
            m_Distance -= zoomAmount;
            m_Distance  = glm::max(m_Distance, 0.1f);
            m_Position  = CalculatePosition();
            updated = true;
        }
        // --- Pan (Middle mouse) ---
        else if (mmb)
        {
            glm::vec3 right = GetRightDirection();
            glm::vec3 up    = GetUpDirection();
            float speed      = m_PanSpeed * m_Distance * dt;

            m_FocalPoint += -right * delta.x * speed;
            m_FocalPoint +=  up    * delta.y * speed;
            m_Position = CalculatePosition();
            updated = true;
        }

        // --- Scroll zoom (when not in flythrough) ---
        if (!m_IsFlying) {
            float zoomDelta = ImGui::GetIO().MouseWheel;
            if (zoomDelta != 0.0f) {
                float adaptiveSpeed = m_ZoomSpeed * m_Distance * dt;
                m_Distance -= zoomDelta * adaptiveSpeed;
                m_Distance  = glm::max(m_Distance, 0.1f);
                m_Position  = CalculatePosition();
                updated = true;
            }
        }

        // --- Entity tracking (Shift+F lock) ---
        if (m_IsTrackingEntity && m_LockedEntity && m_LockedEntity.IsValid()) {
            m_FocalPoint = m_LockedEntity.GetComponent<Component::Transform>().Position;
            m_Position = CalculatePosition();
            updated = true;
        }

        // --- Cursor visibility ---
        if (m_IsFlying && !m_WasFlying)
            ImGui::SetMouseCursor(ImGuiMouseCursor_None);
        else if (!m_IsFlying && m_WasFlying)
            ImGui::SetMouseCursor(ImGuiMouseCursor_Arrow);
        m_WasFlying = m_IsFlying;

        if (updated) UpdateView();
    }

    void EditorCamera::SetViewportSize(float width, float height) {
        m_ViewportWidth = width;
        m_ViewportHeight = height;
        m_AspectRatio = width / height;
        UpdateProjection();
    }

    void EditorCamera::SetFocalPoint(glm::vec3 focalPoint) {
        m_FocalPoint = focalPoint;
        m_Position = CalculatePosition();
        UpdateView();
    }

    void EditorCamera::SetLockedEntity(Entity entity) {
        m_LockedEntity = entity;
        m_IsTrackingEntity = true;
        if (entity && entity.IsValid()) {
            m_FocalPoint = entity.GetComponent<Component::Transform>().Position;
            m_Position = CalculatePosition();
            UpdateView();
        }
    }

    void EditorCamera::ClearLockedEntity() {
        m_LockedEntity = {};
        m_IsTrackingEntity = false;
    }

    glm::vec3 EditorCamera::GetForwardDirection() const {
        float yawRad = glm::radians(m_Yaw);
        float pitchRad = glm::radians(m_Pitch);
        return glm::vec3(
            cos(yawRad) * cos(pitchRad),
            sin(pitchRad),
            sin(yawRad) * cos(pitchRad)
        );
    }

    glm::vec3 EditorCamera::GetRightDirection() const {
        float yawRad = glm::radians(m_Yaw);
        return glm::vec3(-sin(yawRad), 0.0f, cos(yawRad));
    }

    glm::vec3 EditorCamera::GetUpDirection() const {
        glm::vec3 forward = GetForwardDirection();
        glm::vec3 right = GetRightDirection();
        return glm::normalize(glm::cross(right, forward));
    }

    void EditorCamera::UpdateProjection() {
        m_ProjectionMatrix = glm::perspective(
            glm::radians(m_FOV), m_AspectRatio, m_NearClip, m_FarClip);
        // No Y-flip here — applied at GPU uniform upload only (RenderingSystem::UpdateGlobalUniforms)
    }

    void EditorCamera::UpdateView() {
        m_ViewMatrix = glm::lookAt(m_Position, m_FocalPoint, glm::vec3(0.0f, 1.0f, 0.0f));
    }

    glm::vec3 EditorCamera::CalculatePosition() const {
        return m_FocalPoint - GetForwardDirection() * m_Distance;
    }

    glm::quat EditorCamera::GetOrientation() const {
        return glm::quatLookAt(GetForwardDirection(), glm::vec3(0.0f, 1.0f, 0.0f));
    }

    void EditorCamera::ApplySettings(const EditorSettings& s) {
        m_FlySpeed        = s.cameraFlySpeed;
        m_FOV             = s.cameraFOV;
        m_NearClip        = s.cameraNearClip;
        m_FarClip         = s.cameraFarClip;
        m_RotationSpeed   = s.cameraRotationSpeed;
        m_PanSpeed        = s.cameraPanSpeed;
        m_ZoomSpeed       = s.cameraZoomSpeed;
        m_ShiftMultiplier = s.cameraShiftMult;
        UpdateProjection();
    }

    void EditorCamera::SyncToSettings(EditorSettings& s) const {
        s.cameraFlySpeed      = m_FlySpeed;
        s.cameraFOV           = m_FOV;
        s.cameraNearClip      = m_NearClip;
        s.cameraFarClip       = m_FarClip;
        s.cameraRotationSpeed = m_RotationSpeed;
        s.cameraPanSpeed      = m_PanSpeed;
        s.cameraZoomSpeed     = m_ZoomSpeed;
        s.cameraShiftMult     = m_ShiftMultiplier;
    }
}
