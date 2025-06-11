#include "luthpch.h"
#include "luth/editor/panels/RenderPanel.h"
#include "luth/resources/libraries/ShaderLibrary.h"
#include "luth/ECS/Systems.h"
#include "luth/ECS/Systems/RenderingSystem.h"

namespace Luth
{
    RenderPanel::RenderPanel() : m_SelectedMode("Final")
    {
        LH_CORE_INFO("Created Render panel");
    }

    void RenderPanel::OnInit()
    {
        m_RS = Systems::GetSystem<RenderingSystem>();
        if (m_RS) {
            auto* p = m_RS->GetActivePipeline();
            //m_SelectedAttachment = p->GetFinalColorAttachment();
            m_SelectedAttachment = p->GetPass<PostProcessPass>()->GetGBuffer()->GetColorAttachmentID(0);
        }
    }

    void RenderPanel::OnRender()
    {
        if (!m_RS) return;

        ImGui::Begin("Render");

        // Tab selector
        if (ImGui::Button("Model Viewer", ImVec2(ImGui::GetContentRegionAvail().x * 0.5f, 0))) {
            m_SelectedTab = 0;
        }
        ImGui::SameLine();
        if (ImGui::Button("Post Processing", ImVec2(ImGui::GetContentRegionAvail().x, 0))) {
            m_SelectedTab = 1;
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // Show the selected content
        if (m_SelectedTab == 0) {
            // Model Viewer ==========================
            for (auto& [title, modes] : m_Groups) {
                DrawGroup(title, modes);
                ImGui::Separator();
            }
        }
        else {
            // Post process ==========================
            auto* p = m_RS->GetActivePipeline();
            auto g_SSAOPass = p->GetPass<SSAOPass>();
            auto g_PostProcessPass = p->GetPass<PostProcessPass>();

            // SSAO
            if (ImGui::CollapsingHeader("SSAO", ImGuiTreeNodeFlags_DefaultOpen)) {
                float ssaoRadius = g_SSAOPass->GetRadius();
                if (ImGui::SliderFloat("Radius", &ssaoRadius, 0.0f, 10.0f)) {
                    g_SSAOPass->SetRadius(ssaoRadius);
                }

                float ssaoIntensity = g_SSAOPass->GetIntensity();
                if (ImGui::SliderFloat("Intensity", &ssaoIntensity, 0.0f, 1.0f)) {
                    g_SSAOPass->SetIntensity(ssaoIntensity);
                }

                float ssaoBias = g_SSAOPass->GetBias();
                if (ImGui::SliderFloat("Bias", &ssaoBias, 0.05f, 0.5f)) {
                    g_SSAOPass->SetBias(ssaoBias);
                }
            }

            // Bloom
            if (ImGui::CollapsingHeader("Bloom", ImGuiTreeNodeFlags_DefaultOpen)) {
                float bloomThreshold = g_PostProcessPass->GetBloomThreshold();
                if (ImGui::SliderFloat("Threshold", &bloomThreshold, 0.0f, 2.0f)) {
                    g_PostProcessPass->SetBloomThreshold(bloomThreshold);
                }

                float bloomStrength = g_PostProcessPass->GetBloomStrength();
                if (ImGui::SliderFloat("Strength", &bloomStrength, 0.0f, 2.0f)) {
                    g_PostProcessPass->SetBloomStrength(bloomStrength);
                }

                int bloomPasses = g_PostProcessPass->GetBloomPasses();
                if (ImGui::SliderInt("Passes", &bloomPasses, 1, 10)) {
                    g_PostProcessPass->SetBloomPasses(bloomPasses);
                }
            }

            // Tone Mapping
            if (ImGui::CollapsingHeader("Tone Mapping", ImGuiTreeNodeFlags_DefaultOpen)) {
                const char* operators[] = {
                    "Linear", "Reinhard", "Modified Reinhard",
                    "ACES", "Filmic", "Uncharted 2"
                };

                int currentOp = static_cast<int>(g_PostProcessPass->GetToneMapOperator());
                if (ImGui::Combo("Operator", &currentOp, operators, IM_ARRAYSIZE(operators))) {
                    g_PostProcessPass->SetToneMapOperator(static_cast<ToneMapOperator>(currentOp));
                }

                // Exposure control
                float exposure = g_PostProcessPass->GetExposure();
                if (ImGui::SliderFloat("Exposure", &exposure, 0.1f, 5.0f, "%.2f")) {
                    g_PostProcessPass->SetExposure(exposure);
                }

                // Contrast control
                float contrast = g_PostProcessPass->GetContrast();
                if (ImGui::SliderFloat("Contrast", &contrast, 0.5f, 2.0f, "%.2f")) {
                    g_PostProcessPass->SetContrast(contrast);
                }

                // Saturation control
                float saturation = g_PostProcessPass->GetSaturation();
                if (ImGui::SliderFloat("Saturation", &saturation, 0.0f, 2.0f, "%.2f")) {
                    g_PostProcessPass->SetSaturation(saturation);
                }
            }

            // Color Balance
            if (ImGui::CollapsingHeader("Color Balance", ImGuiTreeNodeFlags_DefaultOpen)) {
                glm::vec3 shadows = g_PostProcessPass->GetShadowBalance();
                if (ImGui::ColorEdit3("Shadows", glm::value_ptr(shadows))) {
                    g_PostProcessPass->SetColorBalance(
                        shadows,
                        g_PostProcessPass->GetMidtoneBalance(),
                        g_PostProcessPass->GetHighlightBalance()
                    );
                }

                glm::vec3 midtones = g_PostProcessPass->GetMidtoneBalance();
                if (ImGui::ColorEdit3("Midtones", glm::value_ptr(midtones))) {
                    g_PostProcessPass->SetColorBalance(
                        g_PostProcessPass->GetShadowBalance(),
                        midtones,
                        g_PostProcessPass->GetHighlightBalance()
                    );
                }

                glm::vec3 highlights = g_PostProcessPass->GetHighlightBalance();
                if (ImGui::ColorEdit3("Highlights", glm::value_ptr(highlights))) {
                    g_PostProcessPass->SetColorBalance(
                        g_PostProcessPass->GetShadowBalance(),
                        g_PostProcessPass->GetMidtoneBalance(),
                        highlights
                    );
                }
            }

            // Vignette
            if (ImGui::CollapsingHeader("Vignette", ImGuiTreeNodeFlags_DefaultOpen)) {
                float vignetteAmount = g_PostProcessPass->GetVignetteAmount();
                if (ImGui::SliderFloat("Amount", &vignetteAmount, 0.0f, 1.0f)) {
                    g_PostProcessPass->SetVignetteAmount(vignetteAmount);
                }

                float vignetteHardness = g_PostProcessPass->GetVignetteHardness();
                if (ImGui::SliderFloat("Hardness", &vignetteHardness, 0.0f, 1.0f)) {
                    g_PostProcessPass->SetVignetteHardness(vignetteHardness);
                }
            }

            // Others
            if (ImGui::CollapsingHeader("Others", ImGuiTreeNodeFlags_DefaultOpen)) {
                // Grain
                float grainAmount = g_PostProcessPass->GetGrainAmount();
                if (ImGui::SliderFloat("Grain Amount", &grainAmount, 0.0f, 0.2f, "%.3f")) {
                    g_PostProcessPass->SetGrainAmount(grainAmount);
                }

                // Sharpness
                float sharpness = g_PostProcessPass->GetSharpness();
                if (ImGui::SliderFloat("Sharpness", &sharpness, -1.0f, 1.0f)) {
                    g_PostProcessPass->SetSharpness(sharpness);
                }

                // Chromatic Aberration
                float aberrationOffset = g_PostProcessPass->GetAberrationOffset();
                if (ImGui::SliderFloat("Chromatic Aberration", &aberrationOffset, 0.0f, 0.02f, "%.4f")) {
                    g_PostProcessPass->SetAberrationOffset(aberrationOffset);
                }
            }
        }

        ImGui::End();
    }

    void RenderPanel::DrawGroup(const char* title, const std::vector<const char*>& modes)
    {
        ImGui::TextUnformatted(title);
        ImGui::Spacing();

        // grab pipeline here
        auto* pipeline = m_RS->GetActivePipeline();
        if (!pipeline) return;

        for (auto mode : modes) {
            bool isSelected = (mode == m_SelectedMode);

            // highlight selected
            if (isSelected)
                ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));

            if (ImGui::Button(mode, ImVec2(120, 0))) {
                m_SelectedMode = mode;
                m_SelectedAttachment = pipeline->GetAttachmentByName(m_SelectedMode);
            }

            if (isSelected)
                ImGui::PopStyleColor();
        }
        ImGui::NewLine();
    }

    void RenderPanel::ApplyRenderingSettings()
    {
        //glPolygonMode(GL_FRONT_AND_BACK, m_Wireframe ? GL_LINE : GL_FILL);
        //m_SingleSided ? glEnable(GL_CULL_FACE) : glDisable(GL_CULL_FACE);
		//m_DepthTest ? glEnable(GL_DEPTH_TEST) : glDisable(GL_DEPTH_TEST);
        //glClearColor(m_ClearColor.x, m_ClearColor.y, m_ClearColor.z, m_ClearColor.w);
    }
}
