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

    void RegisterTransform()
    {
        ComponentDrawerOptions opts;
        opts.Removable     = false;
        opts.ShowInAddMenu = false;
        opts.OnCopy = [](Entity e) {
            const auto& t = e.GetComponent<Transform>();
            nlohmann::json j;
            j["position"] = { t.Position.x, t.Position.y, t.Position.z };
            j["rotation"] = { t.Rotation.x, t.Rotation.y, t.Rotation.z };
            j["scale"]    = { t.Scale.x,    t.Scale.y,    t.Scale.z };
            return j.dump();
        };
        opts.OnPaste = [](Entity e, const std::string& data) -> bool {
            try {
                auto j = nlohmann::json::parse(data);
                Transform newT = e.GetComponent<Transform>();
                newT.Position = { j["position"][0], j["position"][1], j["position"][2] };
                newT.Rotation = { j["rotation"][0], j["rotation"][1], j["rotation"][2] };
                newT.Scale    = { j["scale"][0],    j["scale"][1],    j["scale"][2] };
                newT.IsDirty  = true;
                CommandHistory::Execute(std::make_unique<ComponentReplaceCommand<Transform>>(
                    "Paste Transform", e.GetScene(), (entt::entity)e, std::move(newT)));
                return true;
            } catch (...) { return false; }
        };

        ComponentDrawerRegistry::Register<Transform>(
            "Transform",
            [](Entity entity, Transform& transform) {
                if (UI::BeginProperties()) {
                    Scene* scene = entity.GetScene();
                    entt::entity ent = (entt::entity)entity;
                    {
                        auto state = UI::Property("Position", transform.Position);
                        if (state.changed) transform.IsDirty = true;
                        if (state.committed)
                            EXEC_COMPONENT_PROP("Change Position", scene, ent, Transform, Position,
                                UI::ConsumeItemPreEdit<Vec3>(state.itemId), transform.Position);
                    }
                    {
                        auto state = UI::Property("Rotation", transform.Rotation);
                        if (state.changed) transform.IsDirty = true;
                        if (state.committed)
                            EXEC_COMPONENT_PROP("Change Rotation", scene, ent, Transform, Rotation,
                                UI::ConsumeItemPreEdit<Vec3>(state.itemId), transform.Rotation);
                    }
                    {
                        auto state = UI::Property("Scale", transform.Scale, 0.1f, 1.0f);
                        if (state.changed) transform.IsDirty = true;
                        if (state.committed)
                            EXEC_COMPONENT_PROP("Change Scale", scene, ent, Transform, Scale,
                                UI::ConsumeItemPreEdit<Vec3>(state.itemId), transform.Scale);
                    }
                    UI::EndProperties();
                }
            },
            std::move(opts));
    }
}
