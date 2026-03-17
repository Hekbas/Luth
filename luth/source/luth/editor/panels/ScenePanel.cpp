#include "luthpch.h"
#include "luth/editor/panels/ScenePanel.h"
#include "luth/editor/panels/RenderPanel.h"
#include "luth/editor/panels/HierarchyPanel.h"
#include "luth/scene/Components.h"
#include "luth/renderer/Renderer.h"
#include "luth/platform/RenderEvent.h"
#include "luth/utils/ImGuiUtils.h"
#include "luth/utils/LuthIcons.h"
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
        m_SelectedEntity = Editor::GetPanel<HierarchyPanel>()->GetSelectedEntity();
    }

    void ScenePanel::OnRender()
    {
        // Sync selection from Hierarchy
        if (auto hierarchy = Editor::GetPanel<HierarchyPanel>())
            m_SelectedEntity = hierarchy->GetSelectedEntity();

        ImGui::PushFont(Editor::GetFASolid());
        std::string scene = ICON_FA_GAMEPAD + std::string("  Scene");

        if (ImGui::Begin(scene.c_str(), nullptr, ImGuiWindowFlags_NoScrollbar)) {
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

            // Camera Control
            ImGui::SetNavCursorVisible(true);
            if (m_IsHovered) {
                if (ImGui::IsKeyDown(ImGuiKey_F)) {
                    if (m_SelectedEntity) {
                        glm::vec3 newFocus = m_SelectedEntity.GetComponent<Transform>().Position;
                        m_EditorCamera.SetFocalPoint(newFocus);
                    }
                }

                bool rotate = ImGui::IsMouseDown(1);
                bool pan = ImGui::IsMouseDown(2);
                if (rotate || pan) ImGui::SetNavCursorVisible(false);
                m_EditorCamera.OnUpdate(rotate, pan);
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

        float windowWidth = (float)ImGui::GetWindowWidth();
        float windowHeight = (float)ImGui::GetWindowHeight();
        ImGuizmo::SetRect(ImGui::GetWindowPos().x, ImGui::GetWindowPos().y, windowWidth, windowHeight);

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

        // Gizmo Shortcuts
        if (m_IsFocused && !ImGuizmo::IsUsing())
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

    void EditorCamera::OnUpdate(bool rotate, bool pan) {
        //if (ImGui::GetIO().WantCaptureMouse) return;

        float dt = Time::DeltaTime();
        auto [x, y] = ImGui::GetMousePos();
        glm::vec2 currentMousePos(x, y);
        glm::vec2 delta = (currentMousePos - m_LastMousePosition) * 0.002f;
        m_LastMousePosition = currentMousePos;

        bool updated = false;

        // Rotation (Right Mouse)
        if (rotate) {
            m_Yaw += delta.x * m_RotationSpeed * dt;
            m_Pitch -= delta.y * m_RotationSpeed * dt;
            m_Pitch = glm::clamp(m_Pitch, -89.0f, 89.0f);
            m_Position = CalculatePosition();
            updated = true;
        }

        // Panning (Middle Mouse)
        if (pan) {
            glm::vec3 right = GetRightDirection();
            glm::vec3 up = GetUpDirection();
            float speed = m_PanSpeed * m_Distance * dt;

            m_FocalPoint += -right * delta.x * speed;
            m_FocalPoint += up * delta.y * speed;
            m_Position = CalculatePosition();
            updated = true;
        }

        // Zoom (Mouse Scroll)
        float zoomDelta = ImGui::GetIO().MouseWheel;
        if (zoomDelta != 0.0f) {
            float adaptiveSpeed = m_ZoomSpeed * m_Distance * dt;
            m_Distance -= zoomDelta * adaptiveSpeed;
            m_Distance = glm::max(m_Distance, 0.1f);
            m_Position = CalculatePosition();
            updated = true;
        }

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
        CalculatePosition();
        UpdateView();
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
}
