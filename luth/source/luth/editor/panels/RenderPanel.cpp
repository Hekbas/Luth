#include "luthpch.h"
#include "luth/editor/panels/RenderPanel.h"
#include "luth/editor/EditorSettings.h"
#include "luth/scene/Systems.h"
#include "luth/scene/systems/RenderingSystem.h"
#include "luth/renderer/PostProcessSettings.h"
#include "luth/utils/LuthIcons.h"
#include "luth/editor/UI.h"

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
            if (UI::BeginCollapsingHeader("Environment Lighting", true)) {
                auto& settings = Editor::GetSettings();
                if (UI::BeginProperties("EnvLightProps")) {
                    UI::Property("IBL Intensity", settings.iblIntensity, 0.01f, 0.0f, 5.0f);
                    UI::Property("Skybox Intensity", settings.skyboxIntensity, 0.01f, 0.0f, 5.0f);
                    UI::EndProperties();
                }
                UI::EndCollapsingHeader();
            }

            // Ambient Occlusion (GTAO)
            if (UI::BeginCollapsingHeader("Ambient Occlusion (GTAO)", true)) {
                auto& gtao = pp.gtao;
                if (UI::BeginProperties("GTAOProps")) {
                    UI::Property("Enabled", gtao.enabled);
                    UI::Property("Intensity", gtao.intensity, 0.01f, 0.0f, 3.0f);
                    UI::Property("Radius (m)", gtao.radius, 0.01f, 0.05f, 5.0f);
                    UI::Property("Falloff", gtao.falloff, 0.01f, 0.0f, 1.0f);
                    UI::Property("Power", gtao.power, 0.01f, 0.1f, 4.0f);

                    const char* sliceItems[] = { "2", "3", "4", "8" };
                    int sliceIdx =
                        (gtao.sliceCount == 2) ? 0 :
                        (gtao.sliceCount == 3) ? 1 :
                        (gtao.sliceCount == 4) ? 2 : 3;
                    if (UI::PropertyCombo("Slice Count", sliceIdx, sliceItems, IM_ARRAYSIZE(sliceItems))) {
                        const int values[] = { 2, 3, 4, 8 };
                        gtao.sliceCount = values[sliceIdx];
                    }

                    UI::Property("Steps / Slice", gtao.stepsPerSlice, 1, 6);
                    UI::Property("Visualize AO", gtao.visualize);
                    UI::EndProperties();
                }
                UI::EndCollapsingHeader();
            }

            // Bloom
            if (UI::BeginCollapsingHeader("Bloom", true)) {
                if (UI::BeginProperties("BloomProps")) {
                    UI::Property("Threshold", pp.bloomThreshold, 0.01f, 0.0f, 5.0f);
                    UI::Property("Strength", pp.bloomStrength, 0.01f, 0.0f, 2.0f);
                    UI::EndProperties();
                }
                UI::EndCollapsingHeader();
            }

            // Tone Mapping
            if (UI::BeginCollapsingHeader("Tone Mapping", true)) {
                if (UI::BeginProperties("ToneMapProps")) {
                    const char* operators[] = { "Linear", "Reinhard", "ACES", "Uncharted 2" };
                    int currentOp = static_cast<int>(pp.tonemapOp);
                    if (UI::PropertyCombo("Operator", currentOp, operators, IM_ARRAYSIZE(operators)))
                        pp.tonemapOp = static_cast<TonemapOperator>(currentOp);

                    UI::Property("Exposure", pp.exposure, 0.01f, 0.1f, 10.0f);
                    UI::Property("Contrast", pp.contrast, 0.01f, 0.5f, 2.0f);
                    UI::Property("Saturation", pp.saturation, 0.01f, 0.0f, 2.0f);
                    UI::EndProperties();
                }
                UI::EndCollapsingHeader();
            }

            // Color Balance
            if (UI::BeginCollapsingHeader("Color Balance")) {
                if (UI::BeginProperties("ColorBalProps")) {
                    UI::PropertyColor("Shadows", pp.shadowBalance);
                    UI::PropertyColor("Midtones", pp.midtoneBalance);
                    UI::PropertyColor("Highlights", pp.highlightBalance);
                    UI::EndProperties();
                }
                UI::EndCollapsingHeader();
            }

            // Vignette
            if (UI::BeginCollapsingHeader("Vignette")) {
                if (UI::BeginProperties("VignetteProps")) {
                    UI::Property("Amount", pp.vignetteAmount, 0.01f, 0.0f, 1.0f);
                    UI::Property("Hardness", pp.vignetteHardness, 0.01f, 0.0f, 1.0f);
                    UI::EndProperties();
                }
                UI::EndCollapsingHeader();
            }

            // Others
            if (UI::BeginCollapsingHeader("Effects")) {
                if (UI::BeginProperties("EffectsProps")) {
                    UI::Property("Grain", pp.grainAmount, 0.001f, 0.0f, 0.2f);
                    UI::Property("Sharpness", pp.sharpness, 0.01f, -1.0f, 1.0f);
                    UI::Property("Chromatic Aberration", pp.chromaticAberration, 0.001f, 0.0f, 0.02f);
                    UI::EndProperties();
                }
                UI::EndCollapsingHeader();
            }
        }

        ImGui::End();
    }
}
