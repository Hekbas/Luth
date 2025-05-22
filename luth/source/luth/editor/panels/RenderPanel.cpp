#include "luthpch.h"
#include "luth/editor/panels/RenderPanel.h"
#include "luth/resources/libraries/ShaderLibrary.h"
#include "luth/ECS/Systems.h"
#include "luth/ECS/Systems/RenderingSystem.h"
#include "luth/renderer/techniques/ForwardTechnique.h"

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

        // Rendering Technique section
        ImGui::SeparatorText("Rendering Technique");

        if (auto rs = Systems::GetSystem<RenderingSystem>()) {
            const auto& techniques = rs->GetAvailableTechniques();
            const auto currentTech = rs->GetActiveTechnique();

            if (ImGui::BeginCombo("##Technique", currentTech ? currentTech->GetName().c_str() : "None")) {
                for (const auto& [name, tech] : techniques) {
                    bool isSelected = (tech == currentTech);
                    if (ImGui::Selectable(name.c_str(), isSelected)) {
                        rs->SetTechnique(name);
                        m_SelectedAttachment = rs->GetActiveTechnique()->GetFinalColorAttachment();
                    }
                    if (isSelected) {
                        ImGui::SetItemDefaultFocus();
                    }
                }
                ImGui::EndCombo();
            }

            // Framebuffer attachment selection
            auto attachments = currentTech->GetAllAttachments();
            if (!attachments.empty()) {
                static std::string currentAttachment = "Final";

                if (ImGui::BeginCombo("Framebuffer Attachment", currentAttachment.c_str())) {
                    bool isSelected = (currentAttachment == "Final");
                    if (ImGui::Selectable("Final", isSelected)) {
                        currentAttachment = "Final";
                        m_SelectedAttachment = currentTech->GetFinalColorAttachment();
                    }
                    if (isSelected) {
                        ImGui::SetItemDefaultFocus();
                    }

                    // Add all available attachments
                    for (const auto& [name, id] : attachments) {
                        bool isSelected = (name == currentAttachment);
                        if (ImGui::Selectable(name.c_str(), isSelected)) {
                            currentAttachment = name;
                            m_SelectedAttachment = id;
                        }
                        if (isSelected) {
                            ImGui::SetItemDefaultFocus();
                        }
                    }
                    ImGui::EndCombo();
                }
            }

            if (currentTech->GetName() == "Forward") {
                ForwardTechnique ft = static_cast<ForwardTechnique>(*currentTech);
                // Rendeing Settings
                if (ImGui::SliderFloat("SSAO Radius",   &ft.m_SSAORadius,   0.0f, 10.0f));
                if (ImGui::SliderFloat("SSAO Bias",     &ft.m_SSAOBias,     0.0f, 0.5f));
                if (ImGui::SliderFloat("SSAO Strength", &ft.m_SSAOStrength, 0.0f, 10.0f));
            }
        }

        // Shader override section
        ImGui::SeparatorText("Shader Override");
        if (ImGui::Checkbox("##Override Shader", &m_IsShaderOverride)) {
            //m_RenderingSystem.lock()->SetShaderOverride(m_IsShaderOverride, m_ShaderOverride);
        }
        ImGui::SameLine();
        if (auto shader = ShaderLibrary::Get(m_ShaderOverride)) {
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
            if (ImGui::BeginCombo("##Shader", shader->GetName().c_str())) {
                for (const auto& [uuid, s] : ShaderLibrary::GetAllShaders()) {
                    bool selected;
                    if (ImGui::Selectable(s.Shader->GetName().c_str(), &selected)) {
                        m_ShaderOverride = uuid;
                        //m_RenderingSystem.lock()->SetShaderOverride(m_IsShaderOverride, m_ShaderOverride);
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
