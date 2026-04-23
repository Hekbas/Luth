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

                    auto oldColor = dirLight.Color;
                    if (UI::PropertyColor("Color", dirLight.Color))
                        CommandHistory::Execute(std::make_unique<ComponentPropertyCommand<DirectionalLight, Vec3>>(
                            "Change Light Color", scene, ent, &DirectionalLight::Color, oldColor, dirLight.Color));

                    auto oldIntensity = dirLight.Intensity;
                    if (UI::Property("Intensity", dirLight.Intensity, 0.1f, 0.0f, 1000.0f))
                        CommandHistory::Execute(std::make_unique<ComponentPropertyCommand<DirectionalLight, float>>(
                            "Change Light Intensity", scene, ent, &DirectionalLight::Intensity, oldIntensity, dirLight.Intensity));

                    auto oldCastShadows = dirLight.CastShadows;
                    if (UI::Property("Cast Shadows", dirLight.CastShadows))
                        CommandHistory::Execute(std::make_unique<ComponentPropertyCommand<DirectionalLight, bool>>(
                            "Toggle Cast Shadows", scene, ent, &DirectionalLight::CastShadows, oldCastShadows, dirLight.CastShadows));

                    if (dirLight.CastShadows) {
                        UI::Property("Shadow Bias (C0)", dirLight.ShadowBias[0], 0.0001f, 0.0f, 0.05f);
                        UI::Property("Normal Bias (texels)", dirLight.ShadowNormalBias[0], 0.1f, 0.0f, 10.0f);
                        UI::Property("Blend Width", dirLight.CascadeBlendWidth, 0.01f, 0.0f, 1.0f);
                        UI::Property("Show Cascades", dirLight.DebugVisualizeCascades);

                        auto oldOrtho = dirLight.ShadowOrthoSize;
                        if (UI::Property("Shadow Size", dirLight.ShadowOrthoSize, 1.0f, 10.0f, 2000.0f))
                            CommandHistory::Execute(std::make_unique<ComponentPropertyCommand<DirectionalLight, float>>(
                                "Change Shadow Size", scene, ent, &DirectionalLight::ShadowOrthoSize, oldOrtho, dirLight.ShadowOrthoSize));

                        auto oldDist = dirLight.ShadowDistance;
                        if (UI::Property("Shadow Distance", dirLight.ShadowDistance, 1.0f, 10.0f, 2000.0f))
                            CommandHistory::Execute(std::make_unique<ComponentPropertyCommand<DirectionalLight, float>>(
                                "Change Shadow Distance", scene, ent, &DirectionalLight::ShadowDistance, oldDist, dirLight.ShadowDistance));
                    }
                    UI::EndProperties();
                }
            });
    }
}
