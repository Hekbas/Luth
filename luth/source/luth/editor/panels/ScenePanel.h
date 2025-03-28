#pragma once

#include "luth/editor/Editor.h"
#include "luth/scene/Entity.h"

namespace Luth
{
    class Scene;
    class Framebuffer;

    class EditorCamera
    {
    public:
        EditorCamera() = default;
        EditorCamera(float fov, float aspectRatio, float nearClip, float farClip);

        void OnUpdate(float ts);
        //void OnEvent(Event& e);

        const glm::mat4& GetViewMatrix() const { return m_ViewMatrix; }
        const glm::mat4& GetProjectionMatrix() const { return m_ProjectionMatrix; }
        glm::mat4 GetViewProjection() const { return m_ProjectionMatrix * m_ViewMatrix; }

        void SetViewportSize(float width, float height);

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

        glm::vec3 m_Position = { 0.0f, 0.0f, 10.0f };
        glm::vec3 m_FocalPoint = { 0.0f, 0.0f, 0.0f };

        float m_Distance = 10.0f;
        float m_Pitch = 0.0f;
        float m_Yaw = 0.0f;

        float m_ViewportWidth = 1280;
        float m_ViewportHeight = 720;
    };

    class ScenePanel : public Panel
    {
    public:
        ScenePanel();

        void OnInit() override;
        void OnRender() override;

        void SetContext(const std::shared_ptr<Scene>& context) { m_Context = context; }

        std::shared_ptr<Framebuffer> GetFramebuffer() { return m_Framebuffer; }

        bool IsViewportFocused() const { return m_IsFocused; }
        bool IsViewportHovered() const { return m_IsHovered; }

    private:
        std::shared_ptr<Scene> m_Context;
        std::shared_ptr<Framebuffer> m_Framebuffer;
        EditorCamera m_EditorCamera;

        glm::vec2 m_ViewportSize = { 0.0f, 0.0f };
        bool m_IsFocused = false;
        bool m_IsHovered = false;

        // Gizmo state
        Entity m_SelectedEntity;
        //ImGuizmo::OPERATION m_GizmoOperation = ImGuizmo::TRANSLATE;
    };
}
