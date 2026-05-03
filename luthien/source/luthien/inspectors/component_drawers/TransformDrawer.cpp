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
