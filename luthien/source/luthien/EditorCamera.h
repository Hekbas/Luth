#pragma once

#include "luth/scene/Entity.h"


namespace Luth
{
    struct EditorSettings;

    // Orbit-and-fly camera that drives the Scene viewport. ScenePanel input updates it on the main
    // thread between frames; the resulting view and projection matrices feed RenderingSystem each
    // frame through EditorViewportState. Locked-entity mode follows a target Entity's transform.
    class EditorCamera
    {
    public:
        EditorCamera() = default;
        EditorCamera(float fov, float aspectRatio, float nearClip, float farClip);

        void OnUpdate(float dt);

        Vec3 GetPosition() const { return m_Position; }
        bool IsFlying() const { return m_IsFlying; }

        void SetLockedEntity(Entity entity);
        void ClearLockedEntity();
        const Mat4& GetViewMatrix() const { return m_ViewMatrix; }
        const Mat4& GetProjectionMatrix() const { return m_ProjectionMatrix; }
        Mat4 GetViewProjection() const { return m_ProjectionMatrix * m_ViewMatrix; }

        void SetViewportSize(float width, float height);
        void SetFocalPoint(Vec3 focalPoint);
        float GetFlySpeed() const { return m_FlySpeed; }
        void  SetFlySpeed(float speed) { m_FlySpeed = Math::Clamp(speed, 0.1f, 200.0f); }
        float& GetFlySpeedRef() { return m_FlySpeed; }

        Vec3 GetForwardDirection() const;
        Vec3 GetRightDirection() const;
        Vec3 GetUpDirection() const;

        // Settings accessors
        float GetFOV() const { return m_FOV; }
        void  SetFOV(float fov) { m_FOV = fov; UpdateProjection(); }
        float GetNearClip() const { return m_NearClip; }
        void  SetNearClip(float v) { m_NearClip = v; UpdateProjection(); }
        float GetFarClip() const { return m_FarClip; }
        void  SetFarClip(float v) { m_FarClip = v; UpdateProjection(); }
        float GetRotationSpeed() const { return m_RotationSpeed; }
        void  SetRotationSpeed(float v) { m_RotationSpeed = v; }
        float GetPanSpeed() const { return m_PanSpeed; }
        void  SetPanSpeed(float v) { m_PanSpeed = v; }
        float GetZoomSpeed() const { return m_ZoomSpeed; }
        void  SetZoomSpeed(float v) { m_ZoomSpeed = v; }
        float GetShiftMultiplier() const { return m_ShiftMultiplier; }
        void  SetShiftMultiplier(float v) { m_ShiftMultiplier = v; }

        // Ref accessors for ImGui sliders
        float& GetFOVRef() { return m_FOV; }
        float& GetNearClipRef() { return m_NearClip; }
        float& GetFarClipRef() { return m_FarClip; }
        float& GetRotationSpeedRef() { return m_RotationSpeed; }
        float& GetPanSpeedRef() { return m_PanSpeed; }
        float& GetZoomSpeedRef() { return m_ZoomSpeed; }
        float& GetShiftMultiplierRef() { return m_ShiftMultiplier; }

        void ApplySettings(const EditorSettings& s);
        void SyncToSettings(EditorSettings& s) const;

    private:
        void UpdateProjection();
        void UpdateView();
        Vec3 CalculatePosition() const;
        Quat GetOrientation() const;

    private:
        float m_FOV = 70.0f;
        float m_AspectRatio = 1.778f;
        float m_NearClip = 0.1f;
        float m_FarClip = 1000.0f;

        Mat4 m_ViewMatrix;
        Mat4 m_ProjectionMatrix;

        Vec3 m_Position = { 400.0f, 220.0f, 400.0f };
        Vec3 m_FocalPoint = { 0.0f, 50.0f, 0.0f };

        float m_Distance = 10.0f;
        float m_Pitch = 0.0f;
        float m_Yaw = 0.0f;

        float m_ViewportWidth = 1280;
        float m_ViewportHeight = 720;

        // Camera controls
        float m_RotationSpeed = 20000.0f;
        float m_PanSpeed = 200.0f;
        float m_ZoomSpeed = 100.0f;
        Vec2 m_LastMousePosition = { 0.0f, 0.0f };

        // Flythrough
        bool  m_IsFlying        = false;
        bool  m_WasFlying       = false;
        float m_FlySpeed        = 5.0f;
        float m_ShiftMultiplier = 3.0f;

        // Entity tracking (Shift+F)
        Entity m_LockedEntity;
        bool   m_IsTrackingEntity = false;
    };
}
