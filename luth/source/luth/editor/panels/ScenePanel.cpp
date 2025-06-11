#include "luthpch.h"
#include "luth/editor/panels/ScenePanel.h"
#include "luth/editor/panels/RenderPanel.h"
#include "luth/editor/panels/HierarchyPanel.h"
#include "luth/ECS/Components.h"
#include "luth/renderer/Renderer.h"
#include "luth/renderer/Framebuffer.h"
#include "luth/utils/ImGuiUtils.h"
#include "luth/events/RenderEvent.h"
#include "luth/input/Input.h"

namespace Luth
{
    ScenePanel::ScenePanel(std::shared_ptr<RenderingSystem> renderingSystem)
        : m_RenderingSystem(renderingSystem)
    {
        LH_CORE_INFO("Created Scene panel");

        m_EditorCamera = EditorCamera(50, 1.77, 0.1, 10000);

        EventBus::Subscribe<RenderResizeEvent>(BusType::MainThread, [this](Event& e) { 
            HandleRenderResize(e);
        });
    }

    void ScenePanel::OnInit()
    {
        m_SelectedEntity = Editor::GetPanel<HierarchyPanel>()->GetSelectedEntity();
    }

    void ScenePanel::OnRender()
    {
        if (ImGui::Begin("Scene", nullptr, ImGuiWindowFlags_NoScrollbar)) {
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));

            // Viewport sizing
            const Vec2 newSize = ToGlmVec2(ImGui::GetContentRegionAvail());
            if (newSize != m_ViewportSize && newSize.x > 0 && newSize.y > 0) {
                m_ViewportSize = newSize;

                // Update rendering system and camera
                EventBus::Enqueue<RenderResizeEvent>(BusType::MainThread, newSize.x, newSize.y);
            }

            // Get final output from active rendering technique
            if (auto technique = m_RenderingSystem->GetActivePipeline()) {
                i32 textureID = Editor::GetPanel<RenderPanel>()->GetSelectedAttachment();
                if (textureID == -1) textureID = (i32)technique->GetFinalColorAttachment();
                ImGui::Image(textureID, ToImVec2(m_ViewportSize), { 0, 1 }, { 1, 0 });
            }

            // Interaction states
            m_IsFocused = ImGui::IsWindowFocused();
            m_IsHovered = ImGui::IsWindowHovered();

            // Handle gizmos
            //DrawGizmos();

            // Camera Control
            ImGui::SetNavCursorVisible(true);
            if (m_IsHovered) {
                if (ImGui::IsKeyDown(ImGuiKey_F)) {
                    if (m_SelectedEntity) {
                        glm::vec3 newFocus = m_SelectedEntity->GetComponent<Transform>().m_Position;
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
    }

    /*void ScenePanel::SetViewportCamera(const std::shared_ptr<Camera>& camera) {
        m_EditorCamera = camera;
        if (m_EditorCamera) {
            m_EditorCamera->SetViewportSize(m_ViewportSize.x, m_ViewportSize.y);
        }
    }*/

    void ScenePanel::HandleRenderResize(Event& e)
    {
        if (e.IsInCategory(EventCategoryRender)) {
            auto& resizeEvent = static_cast<RenderResizeEvent&>(e);
            m_RenderingSystem->Resize(resizeEvent.GetWidth(), resizeEvent.GetHeight());
            m_EditorCamera.SetViewportSize(resizeEvent.GetWidth(), resizeEvent.GetHeight());
            m_ViewportSize = { resizeEvent.GetWidth(), resizeEvent.GetHeight() };
            e.m_Handled = true;
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
