#include "luthpch.h"
#include "luth/editor/panels/RenderPanel.h"
#include "luth/editor/EditorSettings.h"
#include "luth/scene/Systems.h"
#include "luth/scene/systems/RenderingSystem.h"
#include "luth/renderer/PostProcessSettings.h"
#include "luth/utils/LuthIcons.h"

#include <glm/gtc/type_ptr.hpp>

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
            // TODO: Re-implement with RenderGraph
            m_SelectedAttachment = 0;
        }
    }

    void RenderPanel::OnRender()
    {
        if (!m_RS) return;

        ImGui::PushFont(Editor::GetFASolid());
        std::string render = ICON_FA_FILM + std::string("  Render");
        ImGui::Begin(render.c_str());
        
        // Tab selector
        if (ImGui::Button(ICON_FA_LAYER_GROUP, ImVec2(ImGui::GetContentRegionAvail().x * 0.5f, 0))) {
            m_SelectedTab = 0;
        }
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_SLIDERS, ImVec2(ImGui::GetContentRegionAvail().x, 0))) {
            m_SelectedTab = 1;
        }
        ImGui::PopFont();

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // Show the selected content
        if (m_SelectedTab == 0) {
            // Model Viewer ==========================
            ImGui::Text("Model Viewer Settings - Coming Soon");
        }
        else {
            // Post process ==========================
            auto& pp = m_RS->GetPostProcessSettings();

            // Environment Lighting
            if (ImGui::CollapsingHeader("Environment Lighting", ImGuiTreeNodeFlags_DefaultOpen)) {
                auto& settings = Editor::GetSettings();
                ImGui::SliderFloat("IBL Intensity", &settings.iblIntensity, 0.0f, 5.0f, "%.2f");
                ImGui::SliderFloat("Skybox Intensity", &settings.skyboxIntensity, 0.0f, 5.0f, "%.2f");
            }

            // Bloom
            if (ImGui::CollapsingHeader("Bloom", ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::SliderFloat("Threshold", &pp.bloomThreshold, 0.0f, 5.0f);
                ImGui::SliderFloat("Strength", &pp.bloomStrength, 0.0f, 2.0f);
            }

            // Tone Mapping
            if (ImGui::CollapsingHeader("Tone Mapping", ImGuiTreeNodeFlags_DefaultOpen)) {
                const char* operators[] = { "Linear", "Reinhard", "ACES", "Uncharted 2" };
                int currentOp = static_cast<int>(pp.tonemapOp);
                if (ImGui::Combo("Operator", &currentOp, operators, IM_ARRAYSIZE(operators)))
                    pp.tonemapOp = static_cast<TonemapOperator>(currentOp);

                ImGui::SliderFloat("Exposure", &pp.exposure, 0.1f, 10.0f, "%.2f");
                ImGui::SliderFloat("Contrast", &pp.contrast, 0.5f, 2.0f, "%.2f");
                ImGui::SliderFloat("Saturation", &pp.saturation, 0.0f, 2.0f, "%.2f");
            }

            // Color Balance
            if (ImGui::CollapsingHeader("Color Balance")) {
                ImGui::ColorEdit3("Shadows", glm::value_ptr(pp.shadowBalance));
                ImGui::ColorEdit3("Midtones", glm::value_ptr(pp.midtoneBalance));
                ImGui::ColorEdit3("Highlights", glm::value_ptr(pp.highlightBalance));
            }

            // Vignette
            if (ImGui::CollapsingHeader("Vignette")) {
                ImGui::SliderFloat("Amount", &pp.vignetteAmount, 0.0f, 1.0f);
                ImGui::SliderFloat("Hardness", &pp.vignetteHardness, 0.0f, 1.0f);
            }

            // Others
            if (ImGui::CollapsingHeader("Effects")) {
                ImGui::SliderFloat("Grain", &pp.grainAmount, 0.0f, 0.2f, "%.3f");
                ImGui::SliderFloat("Sharpness", &pp.sharpness, -1.0f, 1.0f);
                ImGui::SliderFloat("Chromatic Aberration", &pp.chromaticAberration, 0.0f, 0.02f, "%.4f");
            }
        }

        ImGui::End();
    }
}
