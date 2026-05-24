#include "lepch.h"
#include "luthien/panels/RenderPanel.h"
#include "luthien/EditorSnapshot.h"
#include "luthien/EditorSettings.h"
#include "luth/scene/systems/SystemRegistry.h"
#include "luth/scene/systems/RenderingSystem.h"
#include "luth/renderer/settings/PostProcessSettings.h"
#include "luthien/widgets/Icons.h"
#include "luthien/widgets/Widgets.h"


namespace Luth
{
    RenderPanel::RenderPanel() : m_SelectedMode("Final")
    {
        m_WindowID = "Render";
        LH_CORE_INFO("Created Render panel");
    }

    void RenderPanel::OnInit()
    {
        m_RS = SystemRegistry::GetSystem<RenderingSystem>();
        if (m_RS) {
            // TODO: Re-implement with RenderGraph
            m_SelectedAttachment = 0;
        }
    }

    void RenderPanel::OnGather(EditorSnapshotBuilder& builder)
    {
        // Settings UI mutates RenderingSystem post-process settings live; nothing
        // meaningful to capture in gather.
        builder.Add<RenderSettingsSnapshot>();
    }

    void RenderPanel::OnDraw(const EditorSnapshot& /*snapshot*/)
    {
        LH_PROFILE_FUNCTION();
        if (!m_RS) return;

        ImGui::PushFont(Editor::GetFASolid());
        std::string render = ICON_FA_FILM + std::string("  Render");
        BeginWindow(render.c_str());
        
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

            // Selection Outline
            if (UI::BeginCollapsingHeader("Selection Outline")) {
                auto& settings = Editor::GetSettings();
                if (UI::BeginProperties("OutlineProps")) {
                    UI::PropertyColor("Color", settings.outlineColor);
                    UI::Property("Width",          settings.outlineWidth,         0.05f, 0.0f, 8.0f);
                    UI::Property("Occluded Alpha", settings.outlineOccludedAlpha, 0.01f, 0.0f, 1.0f);
                    UI::EndProperties();
                }
                UI::EndCollapsingHeader();
            }

            // Editor Grid
            if (UI::BeginCollapsingHeader("Editor Grid")) {
                auto& settings = Editor::GetSettings();
                if (UI::BeginProperties("GridProps")) {
                    UI::PropertyColor("Axis X Color", settings.gridAxisXColor);
                    UI::PropertyColor("Axis Z Color", settings.gridAxisZColor);
                    UI::PropertyColor("Grid Color",   settings.gridColor);
                    UI::Property("Major Scale",    settings.gridMajorScale,    0.05f, 0.1f,  100.0f);
                    UI::Property("Fade Start",     settings.gridFadeStart,     1.0f,  0.0f,  10000.0f);
                    UI::Property("Fade End",       settings.gridFadeEnd,       1.0f,  0.0f,  10000.0f);
                    UI::Property("Line Thickness", settings.gridLineThickness, 0.05f, 0.1f,  4.0f);
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

            // Volumetric Fog — Wronski frustum-voxel fog with temporal accumulation. The master
            // enable toggle lives in EditorSettings → IBL & Skybox; this section is per-feature tuning.
            if (UI::BeginCollapsingHeader("Volumetric Fog", true)) {
                auto& vs = m_RS->GetVolumetricSettings();

                // Quality preset — atlas resolution (Low/Medium/High). Triggers atlas recreation
                // + descriptor re-bind on change.
                if (UI::BeginProperties("VolumetricQuality")) {
                    const char* kQualities[] = { "Low (80x45x64)", "Medium (160x90x128)", "High (240x135x192)" };
                    int qIdx = static_cast<int>(vs.quality);
                    if (UI::PropertyCombo("Quality", qIdx, kQualities, IM_ARRAYSIZE(kQualities)))
                        vs.quality = static_cast<VolumetricSettings::Quality>(qIdx);
                    UI::EndProperties();
                }

                // In-scatter — phase function + multi-scatter + sky cap.
                if (UI::BeginProperties("VolumetricInScatter")) {
                    UI::Property("Anisotropy (g)",      vs.anisotropy,            0.01f, -0.99f, 0.99f);
                    UI::Property("Multi-Scatter",       vs.multiScatterIntensity, 0.01f,  0.0f,  1.0f);
                    UI::Property("Sky Fog Strength",    vs.skyFogStrength,        0.01f,  0.0f,  1.0f);
                    UI::Property("Sun Absorption Steps", vs.sunFogAbsorptionSteps, 0, 16);
                    UI::EndProperties();
                }

                // Temporal accumulation tuning.
                if (UI::BeginProperties("VolumetricTemporal")) {
                    UI::Property("Temporal Blend (alpha)", vs.temporalAlpha, 0.005f, 0.0f, 1.0f);
                    UI::EndProperties();
                }

                // Distance fog — exponential attenuation with camera-to-fragment distance.
                if (UI::BeginProperties("VolumetricDistanceFog")) {
                    UI::Property      ("Distance Fog",     vs.distanceFogEnabled);
                    UI::PropertyColor ("  Color",          vs.distanceFogColor);
                    UI::Property      ("  Density",        vs.distanceFogDensity,    0.001f, 0.0f,  1.0f);
                    UI::Property      ("  Start (m)",      vs.distanceFogStart,      0.5f,   0.0f,  1000.0f);
                    UI::Property      ("  Max Opacity",    vs.distanceFogMaxOpacity, 0.01f,  0.0f,  1.0f);
                    UI::EndProperties();
                }

                // Height fog — exponential attenuation below a reference height.
                if (UI::BeginProperties("VolumetricHeightFog")) {
                    UI::Property      ("Height Fog",       vs.heightFogEnabled);
                    UI::PropertyColor ("  Color",          vs.heightFogColor);
                    UI::Property      ("  Density",        vs.heightFogDensity,     0.001f, 0.0f,  1.0f);
                    UI::Property      ("  Ref Height (m)", vs.heightFogRefHeight,   0.1f,  -1000.0f, 1000.0f);
                    UI::Property      ("  Falloff",        vs.heightFogFalloff,     0.01f,  0.001f, 5.0f);
                    UI::EndProperties();
                }

                // Debug viz tunables — picked up by AddVizPass push-constant each frame.
                if (UI::BeginProperties("VolumetricViz")) {
                    UI::Property("Viz Density Scale",    vs.vizScaleDensity,   0.1f, 0.0f, 50.0f);
                    UI::Property("Viz In-Scatter Scale", vs.vizScaleInScatter, 0.01f, 0.0f, 10.0f);
                    UI::Property("Viz Overlay Opacity",  vs.vizOpacity,        0.01f, 0.0f, 1.0f);
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
