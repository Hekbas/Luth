#include "lepch.h"
#include "luthien/inspectors/ComponentDrawerRegistry.h"
#include "luthien/inspectors/component_drawers/RegisterComponentDrawers.h"
#include "luthien/widgets/Widgets.h"
#include "luthien/commands/Commands.h"
#include "luthien/CommandHistory.h"
#include "luth/scene/Components.h"

namespace Luth::ComponentDrawers
{
    using namespace Component;

    void RegisterDirectionalLight()
    {
        ComponentDrawerRegistry::RegisterSimple<DirectionalLight>(
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
                        UI::Property("Shadow Bias (C0)", dirLight.ShadowBias[0], 0.0001f, 0.0f, 0.05f);
                        UI::Property("Normal Bias (texels)", dirLight.ShadowNormalBias[0], 0.1f, 0.0f, 10.0f);
                        UI::Property("Blend Width", dirLight.CascadeBlendWidth, 0.01f, 0.0f, 1.0f);
                        UI::Property("Show Cascades", dirLight.DebugVisualizeCascades);

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
                    UI::EndProperties();
                }
            });
    }
}
