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

    void RegisterWind()
    {
        ComponentDrawerOptions opts;
        opts.OnCopy = [](Entity e) {
            const auto& v = e.GetComponent<Wind>();
            nlohmann::json j;
            j["enabled"]              = v.enabled;
            j["strengthMultiplier"]   = v.strengthMultiplier;
            j["phaseOffset"]          = v.phaseOffset;
            j["gustMultiplier"]       = v.gustMultiplier;
            j["detailMultiplier"]     = v.detailMultiplier;
            j["useDirectionOverride"] = v.useDirectionOverride;
            j["directionOverride"]    = { v.directionOverride.x, v.directionOverride.y, v.directionOverride.z };
            j["overrideIsWorldSpace"] = v.overrideIsWorldSpace;
            return j.dump();
        };
        opts.OnPaste = [](Entity e, const std::string& data) -> bool {
            try {
                auto j = nlohmann::json::parse(data);
                Wind newV;
                newV.enabled              = j.value("enabled", true);
                newV.strengthMultiplier   = j.value("strengthMultiplier", 1.0f);
                newV.phaseOffset          = j.value("phaseOffset", 0.0f);
                newV.gustMultiplier       = j.value("gustMultiplier", 1.0f);
                newV.detailMultiplier     = j.value("detailMultiplier", 1.0f);
                newV.useDirectionOverride = j.value("useDirectionOverride", false);
                if (j.contains("directionOverride") && j["directionOverride"].is_array() && j["directionOverride"].size() >= 3)
                    newV.directionOverride = { j["directionOverride"][0], j["directionOverride"][1], j["directionOverride"][2] };
                newV.overrideIsWorldSpace = j.value("overrideIsWorldSpace", true);
                CommandHistory::Execute(std::make_unique<ComponentReplaceCommand<Wind>>(
                    "Paste Wind", e.GetScene(), (entt::entity)e, std::move(newV)));
                return true;
            } catch (...) { return false; }
        };

        ComponentDrawerRegistry::Register<Wind>(
            "Wind",
            [](Entity entity, Wind& v) {
                if (UI::BeginProperties("WindProps")) {
                    Scene* scene = entity.GetScene();
                    entt::entity ent = (entt::entity)entity;

                    {
                        auto state = UI::Property("Enabled", v.enabled);
                        if (state.committed)
                            CommandHistory::Execute(std::make_unique<ComponentPropertyCommand<Wind, bool>>(
                                "Toggle Wind Enabled", scene, ent, &Wind::enabled,
                                UI::ConsumeItemPreEdit<bool>(state.itemId), v.enabled));
                    }
                    {
                        auto state = UI::Property("Strength Mult", v.strengthMultiplier, 0.01f, 0.0f, 8.0f);
                        if (state.committed)
                            CommandHistory::Execute(std::make_unique<ComponentPropertyCommand<Wind, float>>(
                                "Change Wind Strength Mult", scene, ent, &Wind::strengthMultiplier,
                                UI::ConsumeItemPreEdit<float>(state.itemId), v.strengthMultiplier));
                    }
                    {
                        auto state = UI::Property("Phase Offset", v.phaseOffset, 0.05f);
                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip("De-syncs distinct deformable meshes that share the global wind.");
                        if (state.committed)
                            CommandHistory::Execute(std::make_unique<ComponentPropertyCommand<Wind, float>>(
                                "Change Wind Phase Offset", scene, ent, &Wind::phaseOffset,
                                UI::ConsumeItemPreEdit<float>(state.itemId), v.phaseOffset));
                    }
                    {
                        auto state = UI::Property("Gust Mult", v.gustMultiplier, 0.01f, 0.0f, 8.0f);
                        if (state.committed)
                            CommandHistory::Execute(std::make_unique<ComponentPropertyCommand<Wind, float>>(
                                "Change Wind Gust Mult", scene, ent, &Wind::gustMultiplier,
                                UI::ConsumeItemPreEdit<float>(state.itemId), v.gustMultiplier));
                    }
                    {
                        auto state = UI::Property("Detail Mult", v.detailMultiplier, 0.01f, 0.0f, 8.0f);
                        if (state.committed)
                            CommandHistory::Execute(std::make_unique<ComponentPropertyCommand<Wind, float>>(
                                "Change Wind Detail Mult", scene, ent, &Wind::detailMultiplier,
                                UI::ConsumeItemPreEdit<float>(state.itemId), v.detailMultiplier));
                    }
                    {
                        auto state = UI::Property("Direction Override", v.useDirectionOverride);
                        if (state.committed)
                            CommandHistory::Execute(std::make_unique<ComponentPropertyCommand<Wind, bool>>(
                                "Toggle Wind Direction Override", scene, ent, &Wind::useDirectionOverride,
                                UI::ConsumeItemPreEdit<bool>(state.itemId), v.useDirectionOverride));
                    }
                    if (v.useDirectionOverride) {
                        {
                            auto state = UI::Property("Override Dir", v.directionOverride, 0.05f);
                            if (ImGui::IsItemHovered())
                                ImGui::SetTooltip("Per-mesh wind axis. Only meaningful for single-instance meshes\n(shared meshes are last-writer-wins).");
                            if (state.committed)
                                CommandHistory::Execute(std::make_unique<ComponentPropertyCommand<Wind, Vec3>>(
                                    "Change Wind Override Dir", scene, ent, &Wind::directionOverride,
                                    UI::ConsumeItemPreEdit<Vec3>(state.itemId), v.directionOverride));
                        }
                        {
                            auto state = UI::Property("Override Is World", v.overrideIsWorldSpace);
                            if (ImGui::IsItemHovered())
                                ImGui::SetTooltip("On = world-space (transformed into object space).\nOff = the override is already in the mesh's object space.");
                            if (state.committed)
                                CommandHistory::Execute(std::make_unique<ComponentPropertyCommand<Wind, bool>>(
                                    "Toggle Wind Override Space", scene, ent, &Wind::overrideIsWorldSpace,
                                    UI::ConsumeItemPreEdit<bool>(state.itemId), v.overrideIsWorldSpace));
                        }
                    }

                    UI::EndProperties();
                }
            },
            std::move(opts));
    }
}
