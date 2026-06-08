#include "lepch.h"
#include "luthien/panels/RenderPanel.h"
#include "luthien/EditorSnapshot.h"
#include "luthien/EditorSettings.h"
#include "luth/scene/systems/SystemRegistry.h"
#include "luth/scene/systems/RenderingSystem.h"
#include "luth/renderer/settings/PostProcessSettings.h"
#include "luth/renderer/settings/RestirSettings.h"
#include "luth/renderer/settings/RestirGiSettings.h"
#include "luth/renderer/settings/SvgfSettings.h"
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
        // Settings UI mutates RenderingSystem post-process settings live; nothing meaningful to capture in gather.
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
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Atlas resolution preset. Higher = sharper fog detail + temporal stability;\nLow uses ~3.5 MB GPU per view, High uses ~50 MB.");
                    UI::EndProperties();
                }

                // In-scatter — phase function + multi-scatter + sky cap.
                if (UI::BeginProperties("VolumetricInScatter")) {
                    UI::Property("Anisotropy (g)", vs.anisotropy, 0.01f, -0.99f, 0.99f);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Henyey-Greenstein phase function parameter. 0 = isotropic;\npositive = forward scatter (god rays); negative = backscatter.\nTypical fog: 0.3-0.7.");
                    UI::Property("Scattering Intensity", vs.scatteringIntensity, 0.5f, 0.0f, 100.0f);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Artistic post-canonical multiplier on total in-scatter (single + multi).\nLifts off-axis fog into visible range against the HG-phase forward bias.\nUE5 / Frostbite expose the same knob. 1.0 = energy-conserving; 10-50 = visible at default scenes.\nIncrease ambient feel by raising; > 1 is intentionally non-physical.");
                    UI::Property("Multi-Scatter", vs.multiScatterIntensity, 0.01f, 0.0f, 1.0f);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("2nd-order multi-scatter — adds IBL ambient as an extinction-weighted in-scatter term.\nLifts shadowed fog (single-scatter alone leaves shadowed regions black).");
                    UI::Property("Sky Fog Strength", vs.skyFogStrength, 0.01f, 0.0f, 1.0f);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Multiplier on fog opacity at sky pixels.\n1.0 = sky fully fogged in dense fog; 0.0 = sky never affected.");
                    UI::Property("Sun Absorption Steps", vs.sunFogAbsorptionSteps, 0, 16);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Steps for sun light-path absorption ray-march through fog.\n0 = disabled; 4 = quality default. Higher = more accurate dense-fog self-shadowing.");
                    UI::EndProperties();
                }

                // Temporal accumulation tuning.
                if (UI::BeginProperties("VolumetricTemporal")) {
                    UI::Property("Temporal Blend (alpha)", vs.temporalAlpha, 0.005f, 0.0f, 1.0f);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Fresh-sample weight in the resolve pass blend.\nWronski recommends 0.05 (95% history) for stable fog under motion.");
                    UI::Property("Blue-Noise Dither", vs.blueNoiseDither);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Jitters per-fragment atlas slice by +/- 0.5 slices using Roberts R2 noise.\nBreaks up Wronski log-slice Z-banding. Requires TAA on to integrate cleanly\n(without TAA you'll see grain).");
                    UI::EndProperties();
                }

                // Density noise — 3D Worley-FBM modulation for the "wispy" look.
                if (UI::BeginProperties("VolumetricNoise")) {
                    UI::Property("Noise Scale",    vs.noiseScale,    0.005f, 0.001f, 1.0f);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("World-space frequency (1/m). Larger = smaller clumps.\nTypical: 0.02 (50m clumps) to 0.1 (10m clumps).");
                    UI::Property("Noise Strength", vs.noiseStrength, 0.01f,  0.0f,   1.0f);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Density modulation amplitude. 0 = disabled, uniform fog;\n1 = swings density 0×..2× around its mean.");
                    UI::Property("Wind (m/s)",     vs.noiseWind,     0.05f);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Wind direction × speed — animates the noise UVW over time\nfor slow atmospheric drift.");
                    UI::EndProperties();
                }

                // Distance fog — exponential attenuation with camera-to-fragment distance.
                if (UI::BeginProperties("VolumetricDistanceFog")) {
                    UI::Property      ("Distance Fog",     vs.distanceFogEnabled);
                    UI::PropertyColor ("  Color",          vs.distanceFogColor);
                    UI::Property      ("  Density",        vs.distanceFogDensity,    0.001f, 0.0f,  1.0f);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Extinction coefficient σ_t (1/m). Beer-Lambert: T = exp(−σ_t · path_length).\nTypical atmospheric haze: 0.005-0.02. Light fog: 0.05-0.1. Dense fog: 0.2+.\nNon-zero values where camDist > Start contribute to per-voxel density.");
                    UI::Property      ("  Start (m)",      vs.distanceFogStart,      0.5f,   0.0f,  1000.0f);
                    UI::Property      ("  Max Opacity",    vs.distanceFogMaxOpacity, 0.01f,  0.0f,  1.0f);
                    UI::EndProperties();
                }

                // Height fog — exponential attenuation below a reference height.
                if (UI::BeginProperties("VolumetricHeightFog")) {
                    UI::Property      ("Height Fog",       vs.heightFogEnabled);
                    UI::PropertyColor ("  Color",          vs.heightFogColor);
                    UI::Property      ("  Density",        vs.heightFogDensity,     0.001f, 0.0f,  1.0f);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Extinction coefficient σ_t (1/m) at the reference height.\nFalls off exponentially above ref. Same scale as distance-fog density.");
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

            // ReSTIR DI — Bitterli20 spatiotemporal reservoir resampling for shadowed point lighting.
            // u32 settings bridge through int locals (UI::Property has no u32 overload); written back
            // only when the drag changes, matching the GTAO slice-combo pattern above.
            if (UI::BeginCollapsingHeader("ReSTIR DI", true)) {
                auto& rs = m_RS->GetRestirSettings();
                if (UI::BeginProperties("RestirProps")) {
                    UI::Property("Enabled", rs.enabled);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Spatiotemporal reservoir resampling for shadowed point lights.\nOff falls back to the unshadowed Forward+ cluster loop. Requires a TLAS (RT).");

                    int candidateCount = static_cast<int>(rs.candidateCount);
                    if (UI::Property("Candidate Count (M)", candidateCount, 1, 64))
                        rs.candidateCount = static_cast<u32>(candidateCount);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Initial RIS candidates sampled per pixel before the visibility ray.\nHigher = less noise, more cost.");

                    int temporalMCap = static_cast<int>(rs.temporalMCap);
                    if (UI::Property("Temporal M-Cap", temporalMCap, 1, 64))
                        rs.temporalMCap = static_cast<u32>(temporalMCap);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("History confidence clamp (prev.M <= cap * curr.M).\nLower = more responsive, more noise; higher = more stable, more lag.");

                    UI::Property("Temporal Depth Threshold", rs.temporalDepthThreshold, 0.005f, 0.0f, 1.0f);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Relative depth tolerance for accepting the reprojected history reservoir.");
                    UI::Property("Temporal Normal Threshold", rs.temporalNormalThreshold, 0.005f, 0.0f, 1.0f);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Min N·N dot to accept the reprojected history reservoir (1 = identical normals).");

                    int spatialNeighbours = static_cast<int>(rs.spatialNeighbours);
                    if (UI::Property("Spatial Neighbours", spatialNeighbours, 0, 16))
                        rs.spatialNeighbours = static_cast<u32>(spatialNeighbours);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Random disk neighbours merged per pixel in the spatial pass.\n0 = spatial reuse off.");

                    int spatialRadius = static_cast<int>(rs.spatialRadius);
                    if (UI::Property("Spatial Radius (px)", spatialRadius, 1, 64))
                        rs.spatialRadius = static_cast<u32>(spatialRadius);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Pixel radius of the spatial neighbour sampling disk.");

                    UI::Property("Spatial Depth Threshold", rs.spatialDepthThreshold, 0.005f, 0.0f, 1.0f);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Relative depth tolerance for accepting a spatial neighbour reservoir.");
                    UI::EndProperties();
                }
                UI::EndCollapsingHeader();
            }

            // ReSTIR GI — Ouyang21 spatiotemporal reservoir resampling for 1-bounce indirect diffuse.
            // Reservoirs store a world-space path sample, so reuse carries a reconnection Jacobian (DI's
            // light-index reservoirs don't). The bounce is added on top of DI under restirParams.y.
            if (UI::BeginCollapsingHeader("ReSTIR GI", true)) {
                auto& gi = m_RS->GetRestirGiSettings();
                if (UI::BeginProperties("RestirGiProps")) {
                    UI::Property("Enabled", gi.enabled);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("1-bounce indirect-diffuse GI via per-pixel path reservoirs.\nAdded on top of direct lighting. Requires a TLAS (RT).");

                    UI::Property("Max Indirect (firefly)", gi.maxIndirect, 0.5f, 0.0f, 100.0f);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Luminance clamp on the secondary-hit radiance before reuse.\nLower kills fireflies harder; higher preserves bright bounces.");

                    int giMCap = static_cast<int>(gi.temporalMCap);
                    if (UI::Property("Temporal M-Cap", giMCap, 1, 64))
                        gi.temporalMCap = static_cast<u32>(giMCap);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("History confidence clamp (prev.M <= cap * curr.M).\nLower = more responsive, more noise; higher = more stable, more lag.");

                    int giMaxAge = static_cast<int>(gi.maxReservoirAge);
                    if (UI::Property("Max Reservoir Age", giMaxAge, 1, 120))
                        gi.maxReservoirAge = static_cast<u32>(giMaxAge);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Discard temporal samples older than this many frames (staleness cap).");

                    UI::Property("Temporal Depth Threshold", gi.temporalDepthThreshold, 0.005f, 0.0f, 1.0f);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Relative depth tolerance for accepting the reprojected history reservoir.");
                    UI::Property("Temporal Normal Threshold", gi.temporalNormalThreshold, 0.005f, 0.0f, 1.0f);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Min N·N dot to accept the reprojected history reservoir (1 = identical normals).");

                    int giNeighbours = static_cast<int>(gi.spatialNeighbours);
                    if (UI::Property("Spatial Neighbours", giNeighbours, 0, 16))
                        gi.spatialNeighbours = static_cast<u32>(giNeighbours);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Random disk neighbours merged per pixel (BASIC bias correction).\n0 = spatial reuse off.");

                    int giRadius = static_cast<int>(gi.spatialRadius);
                    if (UI::Property("Spatial Radius (px)", giRadius, 1, 64))
                        gi.spatialRadius = static_cast<u32>(giRadius);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Pixel radius of the spatial neighbour sampling disk.");

                    UI::Property("Spatial Depth Threshold", gi.spatialDepthThreshold, 0.005f, 0.0f, 1.0f);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Relative depth tolerance for accepting a spatial neighbour reservoir.");
                    UI::Property("Spatial Normal Threshold", gi.spatialNormalThreshold, 0.005f, 0.0f, 1.0f);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Min N·N dot to accept a spatial neighbour reservoir.");

                    UI::Property("Secondary Albedo (scaffold)", gi.secondaryAlbedo, 0.01f, 0.0f, 1.0f);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Constant fallback albedo for the secondary hit — only used when the\nshader's GI_USE_SCAFFOLD_LO debug path is enabled (real material otherwise).");
                    UI::EndProperties();
                }
                UI::EndCollapsingHeader();
            }

            // SVGF Denoiser — Schied17 spatiotemporal variance-guided filter over the demodulated DI.
            // Off passes the raw ReSTIR DI straight through (the A/B compare). The knobs take effect as
            // the reproject / à-trous passes land; the enable toggle + plumbing are live now.
            if (UI::BeginCollapsingHeader("SVGF Denoiser", true)) {
                auto& sv = m_RS->GetSvgfSettings();
                if (UI::BeginProperties("SvgfProps")) {
                    UI::Property("Enabled", sv.enabled);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Denoise the ReSTIR DI signal (Schied 2017 SVGF).\nOff passes the raw ~1-spp DI through unchanged.");

                    UI::Property("Color Alpha", sv.alphaColor, 0.01f, 0.0f, 1.0f);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Temporal EMA blend for color at steady state.\nLower = more accumulation / stability, more lag under motion.");
                    UI::Property("Depth Threshold", sv.depthThreshold, 0.005f, 0.0f, 1.0f);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Relative linear-depth tolerance for accepting reprojected history.");
                    UI::Property("Normal Threshold", sv.normalThreshold, 0.005f, 0.0f, 1.0f);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Min dot(prevN, currN) to accept reprojected history (1 = identical normals).");

                    int atrousIterations = static_cast<int>(sv.atrousIterations);
                    if (UI::Property("A-trous Iterations", atrousIterations, 0, 8))
                        sv.atrousIterations = static_cast<u32>(atrousIterations);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Edge-aware wavelet levels (footprint doubles each level).\nMore = smoother + wider; fewer = sharper + noisier.");

                    int historyCap = static_cast<int>(sv.historyCap);
                    if (UI::Property("History Cap", historyCap, 1, 64))
                        sv.historyCap = static_cast<u32>(historyCap);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Temporal accumulation length clamp (alpha floor = 1/cap).\nHigher = more stable, more ghosting under motion.");

                    UI::Property("Luma Sigma", sv.phiColor, 0.1f, 0.1f, 64.0f);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Luminance edge-stopping sigma - the primary tuning knob.\nLower preserves detail; higher blurs across lighting changes.");
                    UI::Property("Normal Sigma", sv.phiNormal, 1.0f, 1.0f, 256.0f);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Normal edge-stopping exponent (higher = sharper normal edges).");
                    UI::Property("Depth Sigma", sv.phiDepth, 0.05f, 0.0f, 8.0f);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Depth edge-stopping scale (fwidth-normalized).");
                    UI::EndProperties();
                }
                UI::EndCollapsingHeader();
            }

            // SVGF (GI) — second SVGF instance over the demodulated GI bounce. Independent tuning from
            // DI (GI is noisier + lower-frequency): default leans on a shorter history + wider à-trous.
            // Off passes the raw GI through (the denoise-vs-raw A/B), bound to Set 3 b6 either way.
            if (UI::BeginCollapsingHeader("SVGF (GI)", true)) {
                auto& sg = m_RS->GetSvgfGiSettings();
                if (UI::BeginProperties("SvgfGiProps")) {
                    UI::Property("Enabled", sg.enabled);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Denoise the ReSTIR GI bounce (Schied 2017 SVGF).\nOff passes the raw GI through unchanged (the A/B compare).");

                    UI::Property("Color Alpha", sg.alphaColor, 0.01f, 0.0f, 1.0f);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Temporal EMA blend for color at steady state.\nLower = more accumulation / stability, more lag under motion.");
                    UI::Property("Depth Threshold", sg.depthThreshold, 0.005f, 0.0f, 1.0f);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Relative linear-depth tolerance for accepting reprojected history.");
                    UI::Property("Normal Threshold", sg.normalThreshold, 0.005f, 0.0f, 1.0f);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Min dot(prevN, currN) to accept reprojected history (1 = identical normals).");

                    int giAtrous = static_cast<int>(sg.atrousIterations);
                    if (UI::Property("A-trous Iterations", giAtrous, 0, 8))
                        sg.atrousIterations = static_cast<u32>(giAtrous);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Edge-aware wavelet levels (footprint doubles each level).\nGI defaults higher than DI — the bounce tolerates a wider blur.");

                    int giHistCap = static_cast<int>(sg.historyCap);
                    if (UI::Property("History Cap", giHistCap, 1, 64))
                        sg.historyCap = static_cast<u32>(giHistCap);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Temporal accumulation length clamp (alpha floor = 1/cap).\nGI defaults lower than DI — the reservoir already accumulates temporally.");

                    UI::Property("Luma Sigma", sg.phiColor, 0.1f, 0.1f, 64.0f);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Luminance edge-stopping sigma - the primary tuning knob.\nLower preserves detail; higher blurs across lighting changes.");
                    UI::Property("Normal Sigma", sg.phiNormal, 1.0f, 1.0f, 256.0f);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Normal edge-stopping exponent (higher = sharper normal edges).");
                    UI::Property("Depth Sigma", sg.phiDepth, 0.05f, 0.0f, 8.0f);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Depth edge-stopping scale (fwidth-normalized).");
                    UI::EndProperties();
                }
                UI::EndCollapsingHeader();
            }

            // Anti-Aliasing — Karis14 YCoCg-clip TAA + Tokuyoshi19 specular AA. Both gated by their
            // own enable flag; sliders pulled from PostProcessSettings (default-on at sane values).
            if (UI::BeginCollapsingHeader("Anti-Aliasing", true)) {
                if (UI::BeginProperties("TaaProps")) {
                    UI::Property("TAA", pp.taaEnabled);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Karis14 temporal antialiasing with YCoCg-clip recipe + closest-depth\nvelocity dilation + Blackman-Harris reconstruction + luma feedback weight.\nReads motion vectors from the slim G-buffer. Disables jitter when off.");
                    UI::Property("Temporal Blend (alpha)", pp.taaTemporalAlpha, 0.005f, 0.05f, 0.3f);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Current-frame feedback weight (Karis default 0.1 = 90% history).\nLower = smoother + more ghosting; higher = sharper + more flicker.");
                    UI::EndProperties();
                }
                if (UI::BeginProperties("SpecAaProps")) {
                    UI::Property("Specular AA", pp.specularAaEnabled);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Tokuyoshi 2019 - lifts BRDF roughness from screen-space normal\ncurvature variance. Kills high-freq specular sparkle on curved metal\nat glancing angles. No cost on flat surfaces.");
                    UI::Property("Sigma", pp.specularAaSigma, 0.01f, 0.0f, 1.0f);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Variance scale. 0 = no boost; 0.5 = recommended; 1.0 = aggressive.\nHigher values smear specular more under curvature.");
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
                    const char* operators[] = { "Linear", "Reinhard", "ACES", "Uncharted 2", "AgX", "AgX Punchy" };
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
