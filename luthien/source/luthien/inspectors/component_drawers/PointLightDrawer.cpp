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

                    {
                        auto state = UI::PropertyColor("Color", pointLight.Color);
                        if (state.committed)
                            CommandHistory::Execute(std::make_unique<ComponentPropertyCommand<PointLight, Vec3>>(
                                "Change Light Color", scene, ent, &PointLight::Color,
                                UI::ConsumeItemPreEdit<Vec3>(state.itemId), pointLight.Color));
                    }
                    {
                        auto state = UI::Property("Intensity", pointLight.Intensity, 0.1f, 0.0f, 1000.0f);
                        if (state.committed)
                            CommandHistory::Execute(std::make_unique<ComponentPropertyCommand<PointLight, float>>(
                                "Change Light Intensity", scene, ent, &PointLight::Intensity,
                                UI::ConsumeItemPreEdit<float>(state.itemId), pointLight.Intensity));
                    }
                    {
                        auto state = UI::Property("Range", pointLight.Range, 0.1f, 0.0f, 10000.0f);
                        if (state.committed)
                            CommandHistory::Execute(std::make_unique<ComponentPropertyCommand<PointLight, float>>(
                                "Change Light Range", scene, ent, &PointLight::Range,
                                UI::ConsumeItemPreEdit<float>(state.itemId), pointLight.Range));
                    }

                    UI::EndProperties();
                }
            });
    }
}
