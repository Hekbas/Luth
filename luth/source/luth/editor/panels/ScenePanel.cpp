#include "luthpch.h"
#include "luth/editor/panels/ScenePanel.h"
#include "luth/ECS/Components.h"
#include "luth/renderer/Renderer.h"
#include "luth/renderer/Framebuffer.h"
#include "luth/utils/ImGuiUtils.h"
#include "luth/events/RenderEvent.h"

namespace Luth
{
    ScenePanel::ScenePanel(std::shared_ptr<RenderingSystem> renderingSystem)
        : m_RenderingSystem(renderingSystem)
    {
        LH_CORE_INFO("Created Scene panel");

        EventBus::Subscribe<RenderResizeEvent>(BusType::MainThread, [this](Event& e) { 
            HandleRenderResize(e);
        });
    }

    void ScenePanel::OnInit() {}

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

                /*if (m_ViewportCamera) {
                    m_ViewportCamera->SetViewportSize(m_ViewportSize.x, m_ViewportSize.y);
                }*/
            }

            // Get final output from active rendering technique
            if (auto technique = m_RenderingSystem->GetActiveTechnique()) {
                const uint32_t textureID = technique->GetFinalColorAttachment();
                ImGui::Image(textureID, ToImVec2(m_ViewportSize), { 0, 1 }, { 1, 0 });
            }

            // Interaction states
            m_IsFocused = ImGui::IsWindowFocused();
            m_IsHovered = ImGui::IsWindowHovered();

            // Handle gizmos
            //DrawGizmos();

            ImGui::PopStyleVar();
        }
        ImGui::End();
    }

    /*void ScenePanel::SetViewportCamera(const std::shared_ptr<Camera>& camera) {
        m_ViewportCamera = camera;
        if (m_ViewportCamera) {
            m_ViewportCamera->SetViewportSize(m_ViewportSize.x, m_ViewportSize.y);
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
        : m_FOV(fov), m_AspectRatio(aspectRatio),
        m_NearClip(nearClip), m_FarClip(farClip) {
        UpdateProjection();
        UpdateView();
    }

    void EditorCamera::OnUpdate(float ts) {
        //if (Input::IsKeyPressed(Key::LeftAlt)) {
        //    // TODO: Implement camera movement
        //}
    }

    void EditorCamera::UpdateProjection() {
        m_ProjectionMatrix = glm::perspective(
            glm::radians(m_FOV), m_AspectRatio, m_NearClip, m_FarClip);
    }

    void EditorCamera::UpdateView() {
        m_ViewMatrix = glm::lookAt(m_Position, m_FocalPoint, glm::vec3(0, 1, 0));
    }

    void EditorCamera::SetViewportSize(float width, float height) {
        m_AspectRatio = width / height;
        UpdateProjection();
    }
}
