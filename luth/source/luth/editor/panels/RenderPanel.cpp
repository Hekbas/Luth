#include "luthpch.h"
#include "luth/editor/panels/RenderPanel.h"
#include "luth/resources/libraries/ShaderLibrary.h"
#include "luth/scene/Systems.h"

namespace Luth
{
    RenderPanel::RenderPanel()
    {
        LH_CORE_INFO("Created Render panel");
    }

    void RenderPanel::OnInit()
    {
        m_ShaderOverride = UUID(101);
        m_RenderingSystem = Systems::GetSystem<RenderingSystem>();
    }

    void RenderPanel::OnRender()
    {
        ImGui::Begin("Render Settings");

        // Shader override section
        ImGui::SeparatorText("Shader Override");
        if (ImGui::Checkbox("##Override Shader", &m_IsShaderOverride)) {
            m_RenderingSystem.lock()->SetShaderOverride(m_IsShaderOverride, m_ShaderOverride);
        }
        ImGui::SameLine();
        if (auto shader = ShaderLibrary::Get(m_ShaderOverride)) {
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
            if (ImGui::BeginCombo("##Shader", shader->GetName().c_str())) {
                for (const auto& [uuid, s] : ShaderLibrary::GetAllShaders()) {
                    bool selected;
                    if (ImGui::Selectable(s.Shader->GetName().c_str(), &selected)) {
                        m_ShaderOverride = uuid;
                        m_RenderingSystem.lock()->SetShaderOverride(m_IsShaderOverride, m_ShaderOverride);
                    }
                }
                ImGui::EndCombo();
            }
        }

        // Rendering parameters
        ImGui::SeparatorText("Rendering State");
        ImGui::Checkbox("Wireframe Mode", &m_Wireframe);
        ImGui::Checkbox("Backface Culling", &m_BackfaceCulling);
        ImGui::Checkbox("Depth Test", &m_DepthTest);
        ImGui::ColorButton("Clear Color", m_ClearColor);

        // Additional controls
        if (ImGui::Button("Reload Shaders")) {
            // TODO: Implement shader reloading
        }

        ImGui::End();

        ApplyRenderingSettings();
    }

    void RenderPanel::ApplyRenderingSettings()
    {
        // Set OpenGL state based on settings
        /*glPolygonMode(GL_FRONT_AND_BACK, m_Wireframe ? GL_LINE : GL_FILL);

        if (m_BackfaceCulling) glEnable(GL_CULL_FACE);
        else glDisable(GL_CULL_FACE);

        if (m_DepthTest) glEnable(GL_DEPTH_TEST);
        else glDisable(GL_DEPTH_TEST);

        glClearColor(m_clearColor[0], m_clearColor[1], m_clearColor[2], m_clearColor[3]);*/
    }
}
