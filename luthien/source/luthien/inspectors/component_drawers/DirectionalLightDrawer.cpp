#include "lepch.h"
#include "luthien/inspectors/ComponentDrawerRegistry.h"
#include "luthien/inspectors/component_drawers/RegisterComponentDrawers.h"
#include "luthien/widgets/Widgets.h"
#include "luthien/commands/Commands.h"
#include "luthien/CommandHistory.h"
#include "luth/scene/Components.h"

#include <nlohmann/json.hpp>

namespace Luth::ComponentDrawers
{
    using namespace Component;

    void RegisterDirectionalLight()
    {
        ComponentDrawerOptions opts;
        opts.OnCopy = [](Entity e) {
            const auto& dl = e.GetComponent<DirectionalLight>();
            nlohmann::json j;
            j["color"]                  = { dl.Color.x, dl.Color.y, dl.Color.z };
            j["intensity"]              = dl.Intensity;
            j["castShadows"]            = dl.CastShadows;
            j["shadowOrthoSize"]        = dl.ShadowOrthoSize;
            j["shadowDistance"]         = dl.ShadowDistance;
            j["splitLambda"]            = dl.SplitLambda;
            j["shadowBias"]             = { dl.ShadowBias[0], dl.ShadowBias[1], dl.ShadowBias[2], dl.ShadowBias[3] };
            j["shadowNormalBias"]       = { dl.ShadowNormalBias[0], dl.ShadowNormalBias[1], dl.ShadowNormalBias[2], dl.ShadowNormalBias[3] };
            j["stabilizeCascades"]      = dl.StabilizeCascades;
            j["cascadeBlendWidth"]      = dl.CascadeBlendWidth;
            j["debugVisualizeCascades"] = dl.DebugVisualizeCascades;
            j["shadowing"]              = static_cast<int>(dl.Shadowing);
            j["rtOriginEpsilon"]        = dl.RtOriginEpsilon;
            j["rtNormalEpsilon"]        = dl.RtNormalEpsilon;
            return j.dump();
        };
        opts.OnPaste = [](Entity e, const std::string& data) -> bool {
            try {
                auto j = nlohmann::json::parse(data);
                DirectionalLight newDl;
                if (j.contains("color") && j["color"].is_array() && j["color"].size() >= 3)
                    newDl.Color = { j["color"][0], j["color"][1], j["color"][2] };
                newDl.Intensity       = j.value("intensity", 1.0f);
                newDl.CastShadows     = j.value("castShadows", true);
                newDl.ShadowOrthoSize = j.value("shadowOrthoSize", 200.0f);
                newDl.ShadowDistance  = j.value("shadowDistance", 200.0f);
                newDl.SplitLambda     = j.value("splitLambda", 0.5f);
                if (j.contains("shadowBias") && j["shadowBias"].is_array())
                    for (size_t i = 0; i < 4 && i < j["shadowBias"].size(); ++i)
                        newDl.ShadowBias[i] = j["shadowBias"][i];
                if (j.contains("shadowNormalBias") && j["shadowNormalBias"].is_array())
                    for (size_t i = 0; i < 4 && i < j["shadowNormalBias"].size(); ++i)
                        newDl.ShadowNormalBias[i] = j["shadowNormalBias"][i];
                newDl.StabilizeCascades      = j.value("stabilizeCascades", true);
                newDl.CascadeBlendWidth      = j.value("cascadeBlendWidth", 0.2f);
                newDl.DebugVisualizeCascades = j.value("debugVisualizeCascades", false);
                newDl.Shadowing       = static_cast<ShadowingMode>(
                                            j.value("shadowing", static_cast<int>(ShadowingMode::RtShadows)));
                newDl.RtOriginEpsilon = j.value("rtOriginEpsilon", 0.001f);
                newDl.RtNormalEpsilon = j.value("rtNormalEpsilon", 0.05f);
                CommandHistory::Execute(std::make_unique<ComponentReplaceCommand<DirectionalLight>>(
                    "Paste DirectionalLight", e.GetScene(), (entt::entity)e, std::move(newDl)));
                return true;
            } catch (...) { return false; }
        };

        ComponentDrawerRegistry::Register<DirectionalLight>(
            "Directional Light",
            [](Entity entity, DirectionalLight& dirLight) {
                if (UI::BeginProperties()) {
                    Scene* scene = entity.GetScene();
                    entt::entity ent = (entt::entity)entity;

                    {
                        auto state = UI::PropertyColor("Color", dirLight.Color);
                        if (state.committed)
                            CommandHistory::Execute(std::make_unique<ComponentPropertyCommand<DirectionalLight, Vec3>>(
                                "Change Light Color", scene, ent, &DirectionalLight::Color,
                                UI::ConsumeItemPreEdit<Vec3>(state.itemId), dirLight.Color));
                    }
                    {
                        auto state = UI::Property("Intensity", dirLight.Intensity, 0.1f, 0.0f, 1000.0f);
                        if (state.committed)
                            CommandHistory::Execute(std::make_unique<ComponentPropertyCommand<DirectionalLight, float>>(
                                "Change Light Intensity", scene, ent, &DirectionalLight::Intensity,
                                UI::ConsumeItemPreEdit<float>(state.itemId), dirLight.Intensity));
                    }
                    {
                        auto state = UI::Property("Cast Shadows", dirLight.CastShadows);
                        if (state.committed)
                            CommandHistory::Execute(std::make_unique<ComponentPropertyCommand<DirectionalLight, bool>>(
                                "Toggle Cast Shadows", scene, ent, &DirectionalLight::CastShadows,
                                UI::ConsumeItemPreEdit<bool>(state.itemId), dirLight.CastShadows));
                    }

                    if (dirLight.CastShadows) {
                        // Shadow path: RT (default) vs raster CSM compare mode. Mirrors the
                        // TonemapOperator UI pattern in RenderPanel.cpp:265-278.
                        const char* shadowingOptions[] = { "Raster CSM", "RT Shadows" };
                        int currentMode = static_cast<int>(dirLight.Shadowing);
                        if (UI::PropertyCombo("Shadowing", currentMode, shadowingOptions, IM_ARRAYSIZE(shadowingOptions)))
                            dirLight.Shadowing = static_cast<ShadowingMode>(currentMode);

                        if (dirLight.Shadowing == ShadowingMode::RasterCSM) {
                            UI::Property("Shadow Bias (C0)", dirLight.ShadowBias[0], 0.0001f, 0.0f, 0.05f);
                            UI::Property("Normal Bias (texels)", dirLight.ShadowNormalBias[0], 0.1f, 0.0f, 10.0f);
                            UI::Property("Blend Width", dirLight.CascadeBlendWidth, 0.01f, 0.0f, 1.0f);

                            {
                                auto state = UI::Property("Shadow Size", dirLight.ShadowOrthoSize, 1.0f, 10.0f, 2000.0f);
                                if (state.committed)
                                    CommandHistory::Execute(std::make_unique<ComponentPropertyCommand<DirectionalLight, float>>(
                                        "Change Shadow Size", scene, ent, &DirectionalLight::ShadowOrthoSize,
                                        UI::ConsumeItemPreEdit<float>(state.itemId), dirLight.ShadowOrthoSize));
                            }
                            {
                                auto state = UI::Property("Shadow Distance", dirLight.ShadowDistance, 1.0f, 10.0f, 2000.0f);
                                if (state.committed)
                                    CommandHistory::Execute(std::make_unique<ComponentPropertyCommand<DirectionalLight, float>>(
                                        "Change Shadow Distance", scene, ent, &DirectionalLight::ShadowDistance,
                                        UI::ConsumeItemPreEdit<float>(state.itemId), dirLight.ShadowDistance));
                            }
                        }
                        else {
                            // RT-specific: world-space ray origin biasing (Wächter-Binder 2019).
                            // OriginEpsilon = constant offset along sun direction; NormalEpsilon = factor
                            // along geometric normal, modulated by (1-NdotL) for grazing-angle safety.
                            UI::Property("RT Origin Epsilon", dirLight.RtOriginEpsilon, 0.0001f, 0.0f, 0.01f);
                            UI::Property("RT Normal Epsilon", dirLight.RtNormalEpsilon, 0.001f, 0.0f, 0.5f);
                        }

                        // debug viz flag stays exposed in both modes — CSM mode tints cascades;
                        // RT mode reuses the flag for the raw R8 mask overlay (future polish).
                        UI::Property("Show Cascades", dirLight.DebugVisualizeCascades);
                    }
                    UI::EndProperties();
                }
            },
            std::move(opts));
    }
}
