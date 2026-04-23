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

    void RegisterTransform()
    {
        ComponentDrawerOptions opts;
        opts.Removable     = false;
        opts.ShowInAddMenu = false;

        ComponentDrawerRegistry::Register<Transform>(
            "Transform",
            [](Entity entity, Transform& transform) {
                if (UI::BeginProperties()) {
                    Scene* scene = entity.GetScene();
                    entt::entity ent = (entt::entity)entity;
                    {
                        auto old = transform.Position;
                        if (UI::Property("Position", transform.Position)) {
                            EXEC_COMPONENT_PROP("Change Position", scene, ent, Transform, Position, old, transform.Position);
                            transform.IsDirty = true;
                        }
                    }
                    {
                        auto old = transform.Rotation;
                        if (UI::Property("Rotation", transform.Rotation)) {
                            EXEC_COMPONENT_PROP("Change Rotation", scene, ent, Transform, Rotation, old, transform.Rotation);
                            transform.IsDirty = true;
                        }
                    }
                    {
                        auto old = transform.Scale;
                        if (UI::Property("Scale", transform.Scale, 0.1f, 1.0f)) {
                            EXEC_COMPONENT_PROP("Change Scale", scene, ent, Transform, Scale, old, transform.Scale);
                            transform.IsDirty = true;
                        }
                    }
                    UI::EndProperties();
                }
            },
            std::move(opts));
    }
}
