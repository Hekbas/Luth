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

    void RegisterSpotLight()
    {
        ComponentDrawerOptions opts;
        opts.OnCopy = [](Entity e) {
            const auto& sl = e.GetComponent<SpotLight>();
            nlohmann::json j;
            j["color"]     = { sl.Color.x, sl.Color.y, sl.Color.z };
            j["intensity"] = sl.Intensity;
            j["range"]     = sl.Range;
            j["innerCone"] = sl.InnerConeAngleDeg;
            j["outerCone"] = sl.OuterConeAngleDeg;
            return j.dump();
        };
        opts.OnPaste = [](Entity e, const std::string& data) -> bool {
            try {
                auto j = nlohmann::json::parse(data);
                SpotLight newSl;
                if (j.contains("color") && j["color"].is_array() && j["color"].size() >= 3)
                    newSl.Color = { j["color"][0], j["color"][1], j["color"][2] };
                newSl.Intensity         = j.value("intensity", 1.0f);
                newSl.Range             = j.value("range", 350.0f);
                newSl.InnerConeAngleDeg = j.value("innerCone", 25.0f);
                newSl.OuterConeAngleDeg = j.value("outerCone", 45.0f);
                CommandHistory::Execute(std::make_unique<ComponentReplaceCommand<SpotLight>>(
                    "Paste SpotLight", e.GetScene(), (entt::entity)e, std::move(newSl)));
                return true;
            } catch (...) { return false; }
        };

        ComponentDrawerRegistry::Register<SpotLight>(
            "Spot Light",
            [](Entity entity, SpotLight& spotLight) {
                if (UI::BeginProperties()) {
                    Scene* scene = entity.GetScene();
                    entt::entity ent = (entt::entity)entity;

                    {
                        auto state = UI::PropertyColor("Color", spotLight.Color);
                        if (state.committed)
                            CommandHistory::Execute(std::make_unique<ComponentPropertyCommand<SpotLight, Vec3>>(
                                "Change Light Color", scene, ent, &SpotLight::Color,
                                UI::ConsumeItemPreEdit<Vec3>(state.itemId), spotLight.Color));
                    }
                    {
                        auto state = UI::Property("Intensity", spotLight.Intensity, 0.1f, 0.0f, 1000.0f);
                        if (state.committed)
                            CommandHistory::Execute(std::make_unique<ComponentPropertyCommand<SpotLight, float>>(
                                "Change Light Intensity", scene, ent, &SpotLight::Intensity,
                                UI::ConsumeItemPreEdit<float>(state.itemId), spotLight.Intensity));
                    }
                    {
                        auto state = UI::Property("Range", spotLight.Range, 0.1f, 0.0f, 10000.0f);
                        if (state.committed)
                            CommandHistory::Execute(std::make_unique<ComponentPropertyCommand<SpotLight, float>>(
                                "Change Light Range", scene, ent, &SpotLight::Range,
                                UI::ConsumeItemPreEdit<float>(state.itemId), spotLight.Range));
                    }
                    {
                        auto state = UI::Property("Inner Cone", spotLight.InnerConeAngleDeg, 0.5f, 0.0f, 89.9f);
                        if (state.committed)
                            CommandHistory::Execute(std::make_unique<ComponentPropertyCommand<SpotLight, float>>(
                                "Change Inner Cone", scene, ent, &SpotLight::InnerConeAngleDeg,
                                UI::ConsumeItemPreEdit<float>(state.itemId), spotLight.InnerConeAngleDeg));
                    }
                    {
                        auto state = UI::Property("Outer Cone", spotLight.OuterConeAngleDeg, 0.5f, 0.0f, 89.9f);
                        if (state.committed)
                            CommandHistory::Execute(std::make_unique<ComponentPropertyCommand<SpotLight, float>>(
                                "Change Outer Cone", scene, ent, &SpotLight::OuterConeAngleDeg,
                                UI::ConsumeItemPreEdit<float>(state.itemId), spotLight.OuterConeAngleDeg));
                    }

                    UI::EndProperties();
                }
            },
            std::move(opts));
    }
}
