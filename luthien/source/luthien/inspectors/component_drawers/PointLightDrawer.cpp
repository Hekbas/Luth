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

    void RegisterPointLight()
    {
        ComponentDrawerRegistry::RegisterSimple<PointLight>(
            "Point Light",
            [](Entity entity, PointLight& pointLight) {
                if (UI::BeginProperties()) {
                    Scene* scene = entity.GetScene();
                    entt::entity ent = (entt::entity)entity;

                    auto oldColor = pointLight.Color;
                    if (UI::PropertyColor("Color", pointLight.Color))
                        CommandHistory::Execute(std::make_unique<ComponentPropertyCommand<PointLight, Vec3>>(
                            "Change Light Color", scene, ent, &PointLight::Color, oldColor, pointLight.Color));

                    auto oldIntensity = pointLight.Intensity;
                    if (UI::Property("Intensity", pointLight.Intensity, 0.1f, 0.0f, 1000.0f))
                        CommandHistory::Execute(std::make_unique<ComponentPropertyCommand<PointLight, float>>(
                            "Change Light Intensity", scene, ent, &PointLight::Intensity, oldIntensity, pointLight.Intensity));

                    auto oldRange = pointLight.Range;
                    if (UI::Property("Range", pointLight.Range, 0.1f, 0.0f, 10000.0f))
                        CommandHistory::Execute(std::make_unique<ComponentPropertyCommand<PointLight, float>>(
                            "Change Light Range", scene, ent, &PointLight::Range, oldRange, pointLight.Range));

                    UI::EndProperties();
                }
            });
    }
}
