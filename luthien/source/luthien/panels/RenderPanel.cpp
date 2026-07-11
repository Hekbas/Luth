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
    namespace
    {
        // Category tabs, ordered by pipeline stage (light -> denoise -> world -> finish -> reference path).
        // Section bodies gate on the active tab; search mode renders every title-matching section flat.
        enum RenderTab { Tab_Lighting, Tab_Denoise, Tab_Environment, Tab_Post, Tab_PathTrace, Tab_Count };
        const char* const kTabLabels[Tab_Count] = { "Lighting", "Denoise", "Environment", "Post FX", "Path Tracing" };

        // Warning glyph flagging a control that reallocates GPU resources (not just a live-mutated value) on change.
        void ReallocHint(const char* tip)
        {
            ImGui::SameLine();
            ImGui::TextDisabled(ICON_WARNING);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Reallocates GPU resources on change.\n%s", tip);
        }

        void Tip(const char* tip)
        {
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
        }

        // SVGF tunables shared by all four denoiser instances (DI / GI / Specular / DI-Spec).
        void DrawSvgfControls(SvgfSettings& sv, const char* id)
        {
            if (UI::BeginProperties(id)) {
                UI::Property("Enabled", sv.enabled);
                Tip("Denoise this signal (Schied 2017 SVGF). Off passes the raw input through (the A/B compare).");
                UI::Property("Color Alpha", sv.alphaColor, 0.01f, 0.0f, 1.0f);
                Tip("Temporal EMA blend for color at steady state.\nLower = more accumulation / stability, more lag under motion.");
                UI::Property("Moments Alpha", sv.alphaMoments, 0.01f, 0.0f, 1.0f);
                Tip("Temporal EMA blend for the luminance moments (variance estimate)\nthat drives a-trous edge-stopping. Usually tracks Color Alpha.");
                UI::Property("Depth Threshold", sv.depthThreshold, 0.005f, 0.0f, 1.0f);
                Tip("Relative linear-depth tolerance for accepting reprojected history.");
                UI::Property("Normal Threshold", sv.normalThreshold, 0.005f, 0.0f, 1.0f);
                Tip("Min dot(prevN, currN) to accept reprojected history (1 = identical normals).");

                int atrous = static_cast<int>(sv.atrousIterations);
                if (UI::Property("A-trous Iterations", atrous, 0, 8)) sv.atrousIterations = static_cast<u32>(atrous);
                Tip("Edge-aware wavelet levels (footprint doubles each level).\nMore = smoother + wider; fewer = sharper + noisier.");

                int cap = static_cast<int>(sv.historyCap);
                if (UI::Property("History Cap", cap, 1, 64)) sv.historyCap = static_cast<u32>(cap);
                Tip("Temporal accumulation length clamp (alpha floor = 1/cap).\nHigher = more stable, more ghosting under motion.");

                UI::Property("Luma Sigma", sv.phiColor, 0.1f, 0.1f, 64.0f);
                Tip("Luminance edge-stopping sigma - the primary tuning knob.\nLower preserves detail; higher blurs across lighting changes.");
                UI::Property("Normal Sigma", sv.phiNormal, 1.0f, 1.0f, 256.0f);
                Tip("Normal edge-stopping exponent (higher = sharper normal edges).");
                UI::Property("Depth Sigma", sv.phiDepth, 0.05f, 0.0f, 8.0f);
                Tip("Depth edge-stopping scale (fwidth-normalized).");
                UI::EndProperties();
            }
        }
    }

    RenderPanel::RenderPanel()
    {
        m_WindowID = "Render";
        LH_LOG(Editor, info, "Created Render panel");
    }

    void RenderPanel::OnInit()
    {
        m_RS = SystemRegistry::GetSystem<RenderingSystem>();
    }

    void RenderPanel::OnGather(EditorSnapshotBuilder& builder)
    {
        // Settings UI mutates RenderingSystem post-process settings live; nothing to capture in gather.
        builder.Add<RenderSettingsSnapshot>();
    }

    void RenderPanel::OnDraw(const EditorSnapshot& /*snapshot*/)
    {
        LH_PROFILE_FUNCTION();
        if (!m_RS) return;

        ImGui::PushFont(Editor::GetIconRegular());
        std::string title = ICON_FILM + std::string("  Render");
        const bool open = BeginWindow(title.c_str());
        ImGui::PopFont();

        if (open)
        {
            auto& settings = Editor::GetSettings();
            auto& pp       = m_RS->GetPostProcessSettings();

            // ---- Top bar: search only (the Raster/Path-Trace toggle lives in the Scene toolbar) ----
            UI::FilterBox("RenderSearch", m_Filter, sizeof(m_Filter), "Search settings...");
            ImGui::Separator();

            const bool searching = m_Filter[0] != '\0';
            int activeTab = settings.renderPanelTab;
            if (activeTab < 0 || activeTab >= Tab_Count) activeTab = 0;

            if (!searching) {
                if (UI::SegmentedButton("RenderTabs", kTabLabels, Tab_Count, &activeTab))
                    settings.renderPanelTab = activeTab;
                ImGui::Spacing();
            }

            // Section gate: tab mode uses a collapsing header (drawn only on its tab); search mode uses a flat
            // always-open block when the title matches. curHeader tells endSection whether to close a header.
            bool curHeader = false;
            auto beginSection = [&](RenderTab owner, const char* label) -> bool {
                if (searching) {
                    if (!UI::PassesFilter(m_Filter, label)) return false;
                    ImGui::SeparatorText(label);
                    curHeader = false;
                    return true;
                }
                if (activeTab != static_cast<int>(owner)) return false;
                curHeader = true;
                return UI::BeginCollapsingHeader(label, true);
            };
            auto endSection = [&]() { if (curHeader) UI::EndCollapsingHeader(); };

            // ---- Lighting (AO + direct/indirect RT) ----
            if (beginSection(Tab_Lighting, "Ambient Occlusion (GTAO)")) {
                auto& gtao = pp.gtao;
                if (UI::BeginProperties("GTAOProps")) {
                    UI::Property("Enabled", gtao.enabled);
                    UI::Property("Half Resolution", gtao.halfRes);
                    ReallocHint("Trace AO at half-res (XeGTAO default), then upscale. ~4x fewer AO pixels.");
                    UI::Property("Intensity", gtao.intensity, 0.01f, 0.0f, 3.0f);
                    UI::Property("Radius (m)", gtao.radius, 0.01f, 0.05f, 5.0f);
                    UI::Property("Falloff", gtao.falloff, 0.01f, 0.0f, 1.0f);
                    UI::Property("Power", gtao.power, 0.01f, 0.1f, 4.0f);

                    const char* sliceItems[] = { "2", "3", "4", "8" };
                    int sliceIdx = (gtao.sliceCount == 2) ? 0 : (gtao.sliceCount == 3) ? 1 : (gtao.sliceCount == 4) ? 2 : 3;
                    if (UI::PropertyCombo("Slice Count", sliceIdx, sliceItems, IM_ARRAYSIZE(sliceItems))) {
                        const int values[] = { 2, 3, 4, 8 };
                        gtao.sliceCount = values[sliceIdx];
                    }
                    UI::Property("Steps / Slice", gtao.stepsPerSlice, 1, 6);
                    UI::Property("Visualize AO", gtao.visualize);
                    UI::EndProperties();
                }
                endSection();
            }

            if (beginSection(Tab_Lighting, "ReSTIR DI")) {
                auto& rs = m_RS->GetRestirSettings();
                if (UI::BeginProperties("RestirProps")) {
                    UI::Property("Enabled", rs.enabled);
                    Tip("Spatiotemporal reservoir resampling for shadowed point lights.\nOff falls back to the unshadowed Forward+ cluster loop. Requires a TLAS (RT).");
                    UI::Property("Half Resolution", rs.halfResolution);
                    ReallocHint("Trace + denoise DI at half-res, then bilateral-upscale. ~4x fewer rays/denoise pixels.");
                    UI::Property("Specular", rs.specular);
                    Tip("Demodulated point-light specular via a dedicated SVGF channel + combined RIS target.\nOff = diffuse-only DI.");
                    if (rs.specular) {
                        UI::Property("Specular Intensity", rs.specularIntensity, 0.01f, 0.0f, 4.0f);
                        UI::Property("Specular Firefly Clamp", rs.diSpecClamp, 1.0f, 0.0f, 512.0f);
                        Tip("Luminance cap on the demodulated spec lobe. Smooth metals drive D_GGX and 1/(4*NoV)\nto huge values at grazing angles; this bounds the spike before the denoiser smears it.");
                    }

                    int candidateCount = static_cast<int>(rs.candidateCount);
                    if (UI::Property("Candidate Count (M)", candidateCount, 1, 64)) rs.candidateCount = static_cast<u32>(candidateCount);
                    Tip("Initial RIS candidates sampled per pixel before the visibility ray.\nHigher = less noise, more cost.");

                    int temporalMCap = static_cast<int>(rs.temporalMCap);
                    if (UI::Property("Temporal M-Cap", temporalMCap, 1, 64)) rs.temporalMCap = static_cast<u32>(temporalMCap);
                    Tip("History confidence clamp (prev.M <= cap * curr.M).\nLower = more responsive, more noise; higher = more stable, more lag.");

                    UI::Property("Temporal Depth Threshold", rs.temporalDepthThreshold, 0.005f, 0.0f, 1.0f);
                    UI::Property("Temporal Normal Threshold", rs.temporalNormalThreshold, 0.005f, 0.0f, 1.0f);

                    int spatialNeighbours = static_cast<int>(rs.spatialNeighbours);
                    if (UI::Property("Spatial Neighbours", spatialNeighbours, 0, 16)) rs.spatialNeighbours = static_cast<u32>(spatialNeighbours);
                    Tip("Random disk neighbours merged per pixel in the spatial pass. 0 = spatial reuse off.");

                    int spatialRadius = static_cast<int>(rs.spatialRadius);
                    if (UI::Property("Spatial Radius (px)", spatialRadius, 1, 64)) rs.spatialRadius = static_cast<u32>(spatialRadius);
                    UI::Property("Spatial Depth Threshold", rs.spatialDepthThreshold, 0.005f, 0.0f, 1.0f);
                    UI::Property("Spatial Normal Threshold", rs.spatialNormalThreshold, 0.005f, 0.0f, 1.0f);
                    UI::Property("Roughness Threshold", rs.roughnessThreshold, 0.005f, 0.0f, 1.0f);
                    Tip("Spatial reuse rejects neighbours whose roughness differs by more than this.\nStops smooth metals importing diffuse-shaped reservoirs (spec fireflies).");
                    UI::EndProperties();
                }
                endSection();
            }

            if (beginSection(Tab_Lighting, "ReSTIR GI")) {
                auto& gi = m_RS->GetRestirGiSettings();
                if (UI::BeginProperties("RestirGiProps")) {
                    UI::Property("Enabled", gi.enabled);
                    Tip("1-bounce indirect-diffuse GI via per-pixel path reservoirs.\nAdded on top of direct lighting. Requires a TLAS (RT).");
                    UI::Property("Half Resolution", gi.halfResolution);
                    ReallocHint("Trace + denoise GI at half-res, then bilateral-upscale. ~4x fewer rays/denoise pixels.");
                    UI::Property("Max Indirect (firefly)", gi.maxIndirect, 0.5f, 0.0f, 100.0f);
                    Tip("Luminance clamp on the secondary-hit radiance before reuse.\nLower kills fireflies harder; higher preserves bright bounces.");

                    int giMCap = static_cast<int>(gi.temporalMCap);
                    if (UI::Property("Temporal M-Cap", giMCap, 1, 64)) gi.temporalMCap = static_cast<u32>(giMCap);
                    int giMaxAge = static_cast<int>(gi.maxReservoirAge);
                    if (UI::Property("Max Reservoir Age", giMaxAge, 1, 120)) gi.maxReservoirAge = static_cast<u32>(giMaxAge);
                    Tip("Discard temporal samples older than this many frames (staleness cap).");

                    UI::Property("Temporal Depth Threshold", gi.temporalDepthThreshold, 0.005f, 0.0f, 1.0f);
                    UI::Property("Temporal Normal Threshold", gi.temporalNormalThreshold, 0.005f, 0.0f, 1.0f);

                    int giNeighbours = static_cast<int>(gi.spatialNeighbours);
                    if (UI::Property("Spatial Neighbours", giNeighbours, 0, 16)) gi.spatialNeighbours = static_cast<u32>(giNeighbours);
                    int giRadius = static_cast<int>(gi.spatialRadius);
                    if (UI::Property("Spatial Radius (px)", giRadius, 1, 64)) gi.spatialRadius = static_cast<u32>(giRadius);
                    UI::Property("Spatial Depth Threshold", gi.spatialDepthThreshold, 0.005f, 0.0f, 1.0f);
                    UI::Property("Spatial Normal Threshold", gi.spatialNormalThreshold, 0.005f, 0.0f, 1.0f);

                    UI::Property("Secondary Albedo (scaffold)", gi.secondaryAlbedo, 0.01f, 0.0f, 1.0f);
                    Tip("Constant fallback albedo for the secondary hit - only used when the\nshader's GI_USE_SCAFFOLD_LO debug path is enabled (real material otherwise).");
                    UI::EndProperties();
                }
                endSection();
            }

            if (beginSection(Tab_Lighting, "RT Reflections")) {
                auto& rf = m_RS->GetReflectionsSettings();
                if (UI::BeginProperties("ReflectionsProps")) {
                    UI::Property("Enabled", rf.enabled);
                    Tip("Ray-traced specular reflections (supersedes SSR).\nComposited into the specular IBL below the roughness cutoff. Requires a TLAS (RT).");
                    UI::Property("Half Resolution", rf.halfResolution);
                    ReallocHint("Trace + denoise reflections at half-res, then bilateral-upscale. Large GPU win.");
                    UI::Property("Roughness Fade Start", rf.roughnessFadeStart, 0.01f, 0.0f, 1.0f);
                    Tip("Full RT reflection at or below this roughness.");
                    UI::Property("Roughness Fade End", rf.roughnessFadeEnd, 0.01f, 0.0f, 1.0f);
                    Tip("Pure prefiltered-env IBL above this roughness; smoothstep blend between Start and End.");
                    UI::Property("Max Ray Distance", rf.maxRayDistance, 1.0f, 0.0f, 10000.0f);
                    UI::Property("Min Lobe Alpha", rf.minLobeAlpha, 0.0002f, 0.0f, 0.05f);
                    Tip("GGX rough^2 floor. Spreads near-mirror lobes so the 1-spp ray stops point-sampling\nbright hits into fireflies the 3-tap specular denoiser cannot remove.");
                    UI::Property("NEE Clamp", rf.neeClamp, 0.5f, 0.0f, 100.0f);
                    Tip("Luminance cap on the reflection-hit point-light term (the x-light-count spike).\nSun and emission stay unclamped.");
                    UI::Property("Firefly Clamp", rf.fireflyClamp, 1.0f, 0.0f, 256.0f);
                    Tip("Per-ray radiance backstop, after the lobe floor + NEE clamp do the real work.");
                    UI::Property("Denoise", rf.denoise);
                    Tip("Run the specular denoiser. Off = raw 1-spp reflection (the A/B compare).");
                    UI::EndProperties();
                }
                endSection();
            }

            // ---- Denoisers ----
            if (beginSection(Tab_Denoise, "Denoisers (SVGF)")) {
                const char* kChannels[] = { "DI", "GI", "Specular", "DI-Spec" };
                int dt = settings.renderDenoiserTab;
                if (dt < 0 || dt >= IM_ARRAYSIZE(kChannels)) dt = 0;
                if (UI::SegmentedButton("SvgfChannel", kChannels, IM_ARRAYSIZE(kChannels), &dt))
                    settings.renderDenoiserTab = dt;
                ImGui::Spacing();
                switch (dt) {
                    case 0:  DrawSvgfControls(m_RS->GetSvgfSettings(),       "SvgfDI");     break;
                    case 1:  DrawSvgfControls(m_RS->GetSvgfGiSettings(),     "SvgfGI");     break;
                    case 2:  DrawSvgfControls(m_RS->GetSvgfSpecSettings(),   "SvgfSpec");   break;
                    default: DrawSvgfControls(m_RS->GetSvgfDiSpecSettings(), "SvgfDiSpec"); break;
                }
                endSection();
            }

            // ---- Environment ----
            // IBL/Skybox intensity are editor-owned (EditorSettings, also in Preferences) but affect the
            // rendered image, so surfaced here; read live each frame via the viewport-state hook.
            if (beginSection(Tab_Environment, "Image-Based Lighting")) {
                if (UI::BeginProperties("IblProps")) {
                    UI::Property("IBL Intensity", settings.iblIntensity, 0.01f, 0.0f, 8.0f);
                    Tip("Diffuse + specular environment ambient strength.");
                    UI::Property("Skybox Intensity", settings.skyboxIntensity, 0.01f, 0.0f, 8.0f);
                    Tip("Background skybox brightness.");
                    UI::EndProperties();
                }
                if (!settings.skyboxPath.empty())
                    ImGui::TextDisabled("HDR: %s", settings.skyboxPath.c_str());
                endSection();
            }

            if (beginSection(Tab_Environment, "Volumetric Fog")) {
                auto& vs = m_RS->GetVolumetricSettings();
                if (UI::BeginProperties("VolMaster")) {
                    UI::Property("Enabled", settings.enableVolumetricFog);
                    Tip("Master toggle (editor preference). Off skips inject + integrate + composite passes.");
                    UI::EndProperties();
                }
                if (UI::BeginProperties("VolumetricQuality")) {
                    const char* kQualities[] = { "Low (80x45x64)", "Medium (160x90x128)", "High (240x135x192)" };
                    int qIdx = static_cast<int>(vs.quality);
                    if (UI::PropertyCombo("Quality", qIdx, kQualities, IM_ARRAYSIZE(kQualities)))
                        vs.quality = static_cast<VolumetricSettings::Quality>(qIdx);
                    ReallocHint("Atlas resolution preset. High uses ~50 MB GPU per view.");
                    UI::EndProperties();
                }
                if (UI::BeginProperties("VolumetricInScatter")) {
                    UI::Property("Anisotropy (g)", vs.anisotropy, 0.01f, -0.99f, 0.99f);
                    Tip("Henyey-Greenstein phase parameter. 0 = isotropic; positive = forward scatter (god rays).");
                    UI::Property("Scattering Intensity", vs.scatteringIntensity, 0.5f, 0.0f, 100.0f);
                    Tip("Artistic multiplier on total in-scatter. 1.0 = energy-conserving; 10-50 = visible at default scenes.");
                    UI::Property("Multi-Scatter", vs.multiScatterIntensity, 0.01f, 0.0f, 1.0f);
                    Tip("2nd-order multi-scatter - lifts shadowed fog that single-scatter leaves black.");
                    UI::Property("Sky Fog Strength", vs.skyFogStrength, 0.01f, 0.0f, 1.0f);
                    UI::Property("Sun Absorption Steps", vs.sunFogAbsorptionSteps, 0, 16);
                    Tip("Sun light-path absorption ray-march steps. 0 = disabled; 4 = quality default.");
                    UI::Property("RT Shadows", vs.rtShadows);
                    Tip("Ray-traced fog shadows (one ray per froxel per cluster light + sun). Costly - showcase only.");
                    UI::EndProperties();
                }
                if (UI::BeginProperties("VolumetricTemporal")) {
                    UI::Property("Temporal Blend (alpha)", vs.temporalAlpha, 0.005f, 0.0f, 1.0f);
                    Tip("Fresh-sample weight in the resolve blend. Wronski recommends 0.05 (95% history).");
                    UI::Property("Blue-Noise Dither", vs.blueNoiseDither);
                    Tip("Jitters atlas slice with Roberts R2 noise to break Z-banding. Needs TAA on to integrate cleanly.");
                    UI::EndProperties();
                }
                if (UI::BeginProperties("VolumetricNoise")) {
                    UI::Property("Noise Scale",    vs.noiseScale,    0.005f, 0.001f, 1.0f);
                    Tip("World-space frequency (1/m). Larger = smaller clumps.");
                    UI::Property("Noise Strength", vs.noiseStrength, 0.01f,  0.0f,   1.0f);
                    UI::Property("Wind (m/s)",     vs.noiseWind,     0.05f);
                    UI::EndProperties();
                }
                if (UI::BeginProperties("VolumetricDistanceFog")) {
                    UI::Property      ("Distance Fog",     vs.distanceFogEnabled);
                    UI::PropertyColor ("  Color",          vs.distanceFogColor);
                    UI::Property      ("  Density",        vs.distanceFogDensity,    0.001f, 0.0f,  1.0f);
                    Tip("Extinction coefficient (1/m). Haze 0.005-0.02, light fog 0.05-0.1, dense 0.2+.");
                    UI::Property      ("  Start (m)",      vs.distanceFogStart,      0.5f,   0.0f,  1000.0f);
                    UI::Property      ("  Max Opacity",    vs.distanceFogMaxOpacity, 0.01f,  0.0f,  1.0f);
                    UI::EndProperties();
                }
                if (UI::BeginProperties("VolumetricHeightFog")) {
                    UI::Property      ("Height Fog",       vs.heightFogEnabled);
                    UI::PropertyColor ("  Color",          vs.heightFogColor);
                    UI::Property      ("  Density",        vs.heightFogDensity,     0.001f, 0.0f,  1.0f);
                    UI::Property      ("  Ref Height (m)", vs.heightFogRefHeight,   0.1f,  -1000.0f, 1000.0f);
                    UI::Property      ("  Falloff",        vs.heightFogFalloff,     0.01f,  0.001f, 5.0f);
                    UI::EndProperties();
                }
                if (UI::BeginProperties("VolumetricViz")) {
                    UI::Property("Viz Density Scale",    vs.vizScaleDensity,   0.1f, 0.0f, 50.0f);
                    UI::Property("Viz In-Scatter Scale", vs.vizScaleInScatter, 0.01f, 0.0f, 10.0f);
                    UI::Property("Viz Overlay Opacity",  vs.vizOpacity,        0.01f, 0.0f, 1.0f);
                    UI::EndProperties();
                }
                endSection();
            }

            if (beginSection(Tab_Environment, "Wind")) {
                auto& w = m_RS->GetWindSettings();
                if (UI::BeginProperties("WindProps")) {
                    UI::Property("Enabled", w.enabled);
                    Tip("Global wind on static wind-deformable meshes.\nOff = bind pose (the deform compute writes the un-bent vertex).");
                    UI::Property("Direction", w.direction, 0.05f);
                    Tip("Sway direction in WORLD space (transformed into each mesh's object space at dispatch).");
                    UI::Property("Strength", w.strength, 0.01f, 0.0f, 4.0f);
                    UI::Property("Main Bend Scale", w.mainBendScale, 0.01f, 0.0f, 2.0f);
                    Tip("Sway along the wind direction, scaled by vertex height (local +Y).");
                    UI::Property("Detail Scale", w.detailScale, 0.005f, 0.0f, 1.0f);
                    Tip("Per-vertex shimmer along the normal (leaf flutter / cloth folds).");
                    UI::Property("Frequency", w.frequency, 0.05f, 0.0f, 10.0f);
                    UI::Property("Gust Strength", w.gustStrength, 0.01f, 0.0f, 2.0f);
                    UI::Property("Gust Frequency", w.gustFrequency, 0.01f, 0.0f, 4.0f);
                    UI::Property("Turbulence", w.turbulenceAmplitude, 0.005f, 0.0f, 1.0f);
                    UI::Property("Turbulence Frequency", w.turbulenceFrequency, 0.01f, 0.0f, 6.0f);
                    UI::EndProperties();
                }
                endSection();
            }

            // ---- Post FX ----
            if (beginSection(Tab_Post, "Transparency (OIT)")) {
                auto& ts = m_RS->GetTransparencySettings();
                if (UI::BeginProperties("TransparencyProps")) {
                    const char* modeItems[] = { "Sorted (back-to-front)", "OIT (per-pixel linked list)" };
                    int modeIdx = static_cast<int>(ts.mode);
                    if (UI::PropertyCombo("Mode", modeIdx, modeItems, IM_ARRAYSIZE(modeItems)))
                        ts.mode = static_cast<Luth::TransparencyMode>(modeIdx);
                    Tip("Sorted: per-mesh back-to-front alpha blend. OIT: per-pixel linked-list store + exact sorted resolve.");

                    int budget = static_cast<int>(ts.avgLayersBudget);
                    if (UI::Property("OIT Layer Budget (avg)", budget, 1, 16)) ts.avgLayersBudget = static_cast<u32>(budget);
                    ReallocHint("Node pool = w*h*budget*16 B per view (1080p @ 4 = ~133 MB). Reallocates on change.");

                    int resolveK = static_cast<int>(ts.maxResolveK);
                    if (UI::Property("OIT Resolve Layers (K)", resolveK, 1, 16)) ts.maxResolveK = static_cast<u32>(resolveK);
                    Tip("Nearest fragments exact-sorted per pixel; deeper fragments tail-merge into the farthest slot.");
                    UI::EndProperties();
                }
                endSection();
            }

            if (beginSection(Tab_Post, "Anti-Aliasing")) {
                if (UI::BeginProperties("TaaProps")) {
                    UI::Property("TAA", pp.taaEnabled);
                    Tip("Karis14 temporal AA (YCoCg-clip + closest-depth velocity dilation + luma feedback).\nReads slim-G-buffer motion. Disables jitter when off.");
                    UI::Property("Temporal Blend (alpha)", pp.taaTemporalAlpha, 0.005f, 0.05f, 0.3f);
                    Tip("Current-frame feedback weight (Karis default 0.1 = 90% history).\nLower = smoother + more ghosting; higher = sharper + more flicker.");
                    UI::EndProperties();
                }
                if (UI::BeginProperties("SpecAaProps")) {
                    UI::Property("Specular AA", pp.specularAaEnabled);
                    Tip("Tokuyoshi 2019 - lifts BRDF roughness from screen-space normal curvature.\nKills specular sparkle on curved metal at glancing angles.");
                    UI::Property("Sigma", pp.specularAaSigma, 0.01f, 0.0f, 1.0f);
                    UI::EndProperties();
                }
                endSection();
            }

            if (beginSection(Tab_Post, "Bloom")) {
                if (UI::BeginProperties("BloomProps")) {
                    UI::Property("Threshold", pp.bloomThreshold, 0.01f, 0.0f, 5.0f);
                    UI::Property("Strength", pp.bloomStrength, 0.01f, 0.0f, 2.0f);
                    UI::Property("Radius", pp.bloomRadius, 0.01f, 0.0f, 2.0f);
                    UI::EndProperties();
                }
                endSection();
            }

            if (beginSection(Tab_Post, "Tone Mapping")) {
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
                endSection();
            }

            if (beginSection(Tab_Post, "Color Grading")) {
                if (UI::BeginProperties("ColorBalProps")) {
                    UI::PropertyColor("Shadows", pp.shadowBalance);
                    UI::PropertyColor("Midtones", pp.midtoneBalance);
                    UI::PropertyColor("Highlights", pp.highlightBalance);
                    UI::EndProperties();
                }
                endSection();
            }

            if (beginSection(Tab_Post, "Lens")) {
                if (UI::BeginProperties("VignetteProps")) {
                    UI::Property("Vignette Amount", pp.vignetteAmount, 0.01f, 0.0f, 1.0f);
                    UI::Property("Vignette Hardness", pp.vignetteHardness, 0.01f, 0.0f, 1.0f);
                    UI::Property("Grain", pp.grainAmount, 0.001f, 0.0f, 0.2f);
                    UI::Property("Sharpness", pp.sharpness, 0.01f, -1.0f, 1.0f);
                    UI::Property("Chromatic Aberration", pp.chromaticAberration, 0.001f, 0.0f, 0.02f);
                    UI::EndProperties();
                }
                endSection();
            }

            // ---- Path Tracing ----
            if (beginSection(Tab_PathTrace, "Path Tracer")) {
                auto& pt = m_RS->GetPathTraceSettings();
                ImGui::TextDisabled("Activate via the Raster/Path Trace switch at the top.");
                if (UI::BeginProperties("PathTraceProps")) {
                    int spp = static_cast<int>(pt.samplesPerFrame);
                    if (UI::Property("Samples / Frame", spp, 1, 4)) pt.samplesPerFrame = static_cast<u32>(spp);
                    Tip("Paths per pixel each frame. Higher converges faster but a long megakernel risks a GPU TDR.");
                    int mb = static_cast<int>(pt.maxBounces);
                    if (UI::Property("Max Bounces", mb, 1, 16)) pt.maxBounces = static_cast<u32>(mb);
                    int rr = static_cast<int>(pt.rrStartDepth);
                    if (UI::Property("RR Start Depth", rr, 1, 16)) pt.rrStartDepth = static_cast<u32>(rr);
                    Tip("Russian roulette begins after this bounce depth (unbiased path termination).");
                    UI::Property("Firefly Clamp", pt.fireflyClamp, 1.0f, 1.0f, 10000.0f);
                    Tip("Luminance clamp on INDIRECT bounces (primary stays unbiased). High = true ground-truth.");
                    UI::Property("Accumulate", pt.accumulate);
                    Tip("Progressive accumulation (resets on camera/scene change). Off = single-frame noisy preview.");
                    UI::EndProperties();
                }
                endSection();
            }

            // Convergence lives with the Path Tracer settings it operates on (read-out + reset).
            if (beginSection(Tab_PathTrace, "Convergence")) {
                if (ImGui::Button("Reset Accumulation"))
                    m_RS->GetPipeline().GetPathTrace().RequestReset();
                ImGui::SameLine();
                ImGui::Text("%u spp accumulated", m_RS->GetPipeline().GetPathTrace().GetLastSampleCount());
                endSection();
            }
        }

        ImGui::End();
    }
}
