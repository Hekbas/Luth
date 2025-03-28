#include "luthpch.h"
#include "luth/editor/panels/ScenePanel.h"
#include "luth/scene/Components.h"
#include "luth/renderer/Renderer.h"
#include "luth/renderer/Framebuffer.h"
#include "luth/utils/ImGuiUtils.h"

namespace Luth
{
	ScenePanel::ScenePanel() {}

	void ScenePanel::OnInit()
	{
		// Create framebuffer for scene rendering
		Framebuffer::Spec spec;
		spec.Width = 1280;
		spec.Height = 720;
		m_Framebuffer = Framebuffer::Create(spec);

        Renderer::BindFramebuffer(m_Framebuffer);
	}

	void ScenePanel::OnRender()
	{
		if (ImGui::Begin("Scene"))
		{
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
            ImGui::Begin("Scene");

            // Store viewport size for later use
            m_ViewportSize = ToGlmVec2(ImGui::GetContentRegionAvail());

            // Render framebuffer to viewport
            uint32_t textureID = m_Framebuffer->GetColorAttachmentRendererID();
            ImGui::Image(textureID,
                ToImVec2(m_ViewportSize),
                ImVec2(0, 1),
                ImVec2(1, 0));

            // Handle viewport focus
            m_IsFocused = ImGui::IsWindowFocused();
            m_IsHovered = ImGui::IsWindowHovered();

            // Handle gizmos
            //DrawGizmos();

            ImGui::End();
            ImGui::PopStyleVar();
		}
		ImGui::End();
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
