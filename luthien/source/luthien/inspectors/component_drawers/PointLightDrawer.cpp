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

    void RegisterPointLight()
    {
        ComponentDrawerOptions opts;
        opts.OnCopy = [](Entity e) {
            const auto& pl = e.GetComponent<PointLight>();
            nlohmann::json j;
            j["color"]     = { pl.Color.x, pl.Color.y, pl.Color.z };
            j["intensity"] = pl.Intensity;
            j["range"]     = pl.Range;
            return j.dump();
        };
        opts.OnPaste = [](Entity e, const std::string& data) -> bool {
            try {
                auto j = nlohmann::json::parse(data);
                PointLight newPl;
                if (j.contains("color") && j["color"].is_array() && j["color"].size() >= 3)
                    newPl.Color = { j["color"][0], j["color"][1], j["color"][2] };
                newPl.Intensity = j.value("intensity", 1.0f);
                newPl.Range     = j.value("range", 350.0f);
                CommandHistory::Execute(std::make_unique<ComponentReplaceCommand<PointLight>>(
                    "Paste PointLight", e.GetScene(), (entt::entity)e, std::move(newPl)));
                return true;
            } catch (...) { return false; }
        };

        ComponentDrawerRegistry::Register<PointLight>(
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
            },
            std::move(opts));
    }
}
