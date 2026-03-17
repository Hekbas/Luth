#pragma once

#include "luth/editor/Editor.h"
#include "luth/scene/Entity.h"
#include "luth/scene/systems/RenderingSystem.h"
#include "luth/platform/Event.h"
#include "luth/platform/EventBus.h"

#include <vulkan/vulkan.h>
#include <ImGuizmo.h>

namespace Luth
{
    class Scene;
    class EditorCamera
    {
    public:
        EditorCamera() = default;
        EditorCamera(float fov, float aspectRatio, float nearClip, float farClip);

        void OnUpdate(float dt);

        glm::vec3 GetPosition() const { return m_Position; }
        bool IsFlying() const { return m_IsFlying; }

        void SetLockedEntity(Entity entity);
        void ClearLockedEntity();
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
        float m_FOV = 70.0f;
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

        // Flythrough
        bool  m_IsFlying        = false;
        bool  m_WasFlying       = false;
        float m_FlySpeed        = 5.0f;
        float m_ShiftMultiplier = 3.0f;

        // Entity tracking (Shift+F)
        Entity m_LockedEntity;
        bool   m_IsTrackingEntity = false;
    };

    class ScenePanel : public Panel
    {
    public:
        ScenePanel(std::shared_ptr<RenderingSystem> renderingSystem);
        ~ScenePanel() override;

        void OnInit() override;
        void OnRender() override;

        void SetContext(const std::shared_ptr<Scene>& context) { m_Context = context; }

        bool IsViewportFocused() const { return m_IsFocused; }
        bool IsViewportHovered() const { return m_IsHovered; }

        EditorCamera& GetEditorCamera() { return m_EditorCamera; }

    private:
        void HandleRenderResize(Event& e);
        void DrawGizmos();

        std::shared_ptr<Scene> m_Context;
        std::shared_ptr<RenderingSystem> m_RenderingSystem;
        EditorCamera m_EditorCamera;

        Vec2 m_ViewportSize = { 0.0f, 0.0f };
        bool m_IsFocused = false;
        bool m_IsHovered = false;

        // Scene viewport texture tracking (must not be static — leaks on shutdown)
        VkDescriptorSet m_SceneDS = VK_NULL_HANDLE;
        std::shared_ptr<Texture> m_LastSceneTex = nullptr;

        // Gizmo state
        Entity m_SelectedEntity;
        int m_GizmoType = -1; // -1 = None, or ImGuizmo::OPERATION
    };
}
