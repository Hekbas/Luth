#include "lepch.h"
#include "luthien/EditorCamera.h"
#include "luthien/EditorSettings.h"
#include "luth/scene/Components.h"

#include <imgui.h>

namespace Luth
{
    EditorCamera::EditorCamera(float fov, float aspectRatio, float nearClip, float farClip)
        : m_FOV(fov), m_AspectRatio(aspectRatio), m_NearClip(nearClip), m_FarClip(farClip)
    {
        Vec3 direction = Math::Normalize(m_FocalPoint - m_Position);
        m_Distance = Math::Length(m_FocalPoint - m_Position);

        m_Yaw = Math::Degrees(atan2(direction.z, direction.x));
        m_Pitch = Math::Degrees(asin(direction.y));

        UpdateProjection();
        UpdateView();
    }

    void EditorCamera::OnUpdate(float dt) {
        auto [x, y] = ImGui::GetMousePos();
        Vec2 currentMousePos(x, y);
        Vec2 delta = (currentMousePos - m_LastMousePosition) * 0.002f;
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
            m_Pitch  = Math::Clamp(m_Pitch, -89.0f, 89.0f);

            // WASD + QE movement
            Vec3 forward = GetForwardDirection();
            Vec3 right   = GetRightDirection();
            Vec3 worldUp = Vec3(0.0f, 1.0f, 0.0f);

            float speed = m_FlySpeed * (shiftHeld ? m_ShiftMultiplier : 1.0f) * dt;
            Vec3 movement(0.0f);

            if (ImGui::IsKeyDown(ImGuiKey_W)) movement += forward;
            if (ImGui::IsKeyDown(ImGuiKey_S)) movement -= forward;
            if (ImGui::IsKeyDown(ImGuiKey_D)) movement += right;
            if (ImGui::IsKeyDown(ImGuiKey_A)) movement -= right;
            if (ImGui::IsKeyDown(ImGuiKey_E)) movement += worldUp;
            if (ImGui::IsKeyDown(ImGuiKey_Q)) movement -= worldUp;

            if (Math::Length(movement) > 0.0001f)
                m_Position += Math::Normalize(movement) * speed;

            // Scroll adjusts base fly speed while in flythrough
            float scrollDelta = ImGui::GetIO().MouseWheel;
            if (scrollDelta != 0.0f) {
                m_FlySpeed += scrollDelta * 0.5f;
                m_FlySpeed = Math::Clamp(m_FlySpeed, 0.1f, 200.0f);
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
            m_Pitch  = Math::Clamp(m_Pitch, -89.0f, 89.0f);
            m_Position = CalculatePosition();
            updated = true;
        }
        // --- Smooth zoom (Alt + RMB drag) ---
        else if (altHeld && rmb)
        {
            float zoomAmount = (delta.x + delta.y) * m_ZoomSpeed * m_Distance * dt;
            m_Distance -= zoomAmount;
            m_Distance  = Math::Max(m_Distance, 0.1f);
            m_Position  = CalculatePosition();
            updated = true;
        }
        // --- Pan (Middle mouse) ---
        else if (mmb)
        {
            Vec3 right = GetRightDirection();
            Vec3 up    = GetUpDirection();
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
                m_Distance  = Math::Max(m_Distance, 0.1f);
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

    void EditorCamera::SetFocalPoint(Vec3 focalPoint) {
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

    Vec3 EditorCamera::GetForwardDirection() const {
        float yawRad = Math::Radians(m_Yaw);
        float pitchRad = Math::Radians(m_Pitch);
        return Vec3(
            cos(yawRad) * cos(pitchRad),
            sin(pitchRad),
            sin(yawRad) * cos(pitchRad)
        );
    }

    Vec3 EditorCamera::GetRightDirection() const {
        float yawRad = Math::Radians(m_Yaw);
        return Vec3(-sin(yawRad), 0.0f, cos(yawRad));
    }

    Vec3 EditorCamera::GetUpDirection() const {
        Vec3 forward = GetForwardDirection();
        Vec3 right = GetRightDirection();
        return Math::Normalize(Math::Cross(right, forward));
    }

    void EditorCamera::UpdateProjection() {
        m_ProjectionMatrix = Math::Perspective(
            Math::Radians(m_FOV), m_AspectRatio, m_NearClip, m_FarClip);
        // No Y-flip here — applied at GPU uniform upload only (RenderingSystem::UpdateGlobalUniforms)
    }

    void EditorCamera::UpdateView() {
        m_ViewMatrix = Math::LookAt(m_Position, m_FocalPoint, Vec3(0.0f, 1.0f, 0.0f));
    }

    Vec3 EditorCamera::CalculatePosition() const {
        return m_FocalPoint - GetForwardDirection() * m_Distance;
    }

    Quat EditorCamera::GetOrientation() const {
        return Math::QuatLookAt(GetForwardDirection(), Vec3(0.0f, 1.0f, 0.0f));
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

    EditorCameraPose EditorCamera::CapturePose() const {
        return { m_FocalPoint, m_Distance, m_Pitch, m_Yaw };
    }

    void EditorCamera::ApplyPose(const EditorCameraPose& pose) {
        // The Shift+F lock targets an entity from the outgoing scene — drop it so OnUpdate
        // can't snap the focal point to a stale handle once the new scene is live.
        ClearLockedEntity();
        m_FocalPoint = pose.focalPoint;
        m_Distance   = Math::Max(pose.distance, 0.1f);
        m_Pitch      = Math::Clamp(pose.pitch, -89.0f, 89.0f);
        m_Yaw        = pose.yaw;
        m_Position   = CalculatePosition();
        UpdateView();
    }
}
