#pragma once

#include "luth/editor/Editor.h"
#include "luth/ECS/Entity.h"
#include "luth/ECS/systems/RenderingSystem.h"
#include "luth/events/Event.h"
#include "luth/events/EventBus.h"

namespace Luth
{
    class Scene;
    class Framebuffer;

    class EditorCamera
    {
    public:
        EditorCamera() = default;
        EditorCamera(float fov, float aspectRatio, float nearClip, float farClip);

        void OnUpdate(bool rotate, bool pan);

        glm::vec3 GetPosition() const { return CalculatePosition(); }
        const glm::mat4& GetViewMatrix() const { return m_ViewMatrix; }
        const glm::mat4& GetProjectionMatrix() const { return m_ProjectionMatrix; }
        glm::mat4 GetViewProjection() const { return m_ProjectionMatrix * m_ViewMatrix; }

        void SetViewportSize(float width, float height);
        void SetFocalPoint(glm::vec3 focalPoint);

        glm::vec3 GetForwardDirection() const;
        glm::vec3 GetRightDirection() const;
        glm::vec3 GetUpDirection() const;

    private:
        void UpdateProjection();
        void UpdateView();
        glm::vec3 CalculatePosition() const;
        glm::quat GetOrientation() const;

    private:
        float m_FOV = 45.0f;
        float m_AspectRatio = 1.778f;
        float m_NearClip = 0.1f;
        float m_FarClip = 1000.0f;

        glm::mat4 m_ViewMatrix;
        glm::mat4 m_ProjectionMatrix;

        glm::vec3 m_Position = { 400.0f, 220.0f, 400.0f };
        glm::vec3 m_FocalPoint = { 0.0f, 50.0f, 0.0f };

        float m_Distance = 10.0f;
        float m_Pitch = 0.0f;
        float m_Yaw = 0.0f;

        float m_ViewportWidth = 1280;
        float m_ViewportHeight = 720;

        // Camera controls
        float m_RotationSpeed = 20000.0f;
        float m_PanSpeed = 200.0f;
        float m_ZoomSpeed = 100.0f;
        glm::vec2 m_LastMousePosition = { 0.0f, 0.0f };
    };

    class ScenePanel : public Panel
    {
    public:
        ScenePanel(std::shared_ptr<RenderingSystem> renderingSystem);

        void OnInit() override;
        void OnRender() override;

        void SetContext(const std::shared_ptr<Scene>& context) { m_Context = context; }

        bool IsViewportFocused() const { return m_IsFocused; }
        bool IsViewportHovered() const { return m_IsHovered; }

        EditorCamera GetEditorCamera() { return m_EditorCamera; }

    private:
        void HandleRenderResize(Event& e);

        std::shared_ptr<Scene> m_Context;
        std::shared_ptr<RenderingSystem> m_RenderingSystem;
        EditorCamera m_EditorCamera;

        Vec2 m_ViewportSize = { 0.0f, 0.0f };
        bool m_IsFocused = false;
        bool m_IsHovered = false;

        // Gizmo state
        Entity* m_SelectedEntity;
        //ImGuizmo::OPERATION m_GizmoOperation = ImGuizmo::TRANSLATE;
    };
}
