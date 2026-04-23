#include "lepch.h"
#include "luthien/inspectors/ComponentDrawerRegistry.h"
#include "luthien/inspectors/component_drawers/RegisterComponentDrawers.h"
#include "luth/scene/Components.h"

#if defined(DEBUG)
namespace Luth::ComponentDrawers
{
    using namespace Component;

    void RegisterID()
    {
        ComponentDrawerOptions opts;
        opts.Removable     = false;
        opts.ShowInAddMenu = false;

        ComponentDrawerRegistry::Register<ID>(
            "ID",
            [](Entity entity, ID& component) {
                ImGui::Text("ID: %llu", component.Value);
            },
            std::move(opts));
    }

    void RegisterParent()
    {
        ComponentDrawerOptions opts;
        opts.ShowInAddMenu = false;

        ComponentDrawerRegistry::Register<Parent>(
            "Parent",
            [](Entity entity, Parent& component) {
                if (component.Value && component.Value.IsValid()) {
                    ImGui::Text("Parent: %s", component.Value.GetName().c_str());
                    if (ImGui::Button("Clear Parent")) {
                        entity.SetParent({});
                    }
                }
                else {
                    ImGui::Text("No Parent");
                }
            },
            std::move(opts));
    }

    void RegisterChildren()
    {
        ComponentDrawerOptions opts;
        opts.ShowInAddMenu = false;

        ComponentDrawerRegistry::Register<Children>(
            "Children",
            [](Entity entity, Children& component) {
                ImGui::Text("Children: %d", component.Value.size());
                for (auto& child : component.Value) {
                    if (child.IsValid()) {
                        ImGui::BulletText("%s", child.GetName().c_str());
                    }
                    else {
                        ImGui::BulletText("Invalid Entity");
                    }
                }
            },
            std::move(opts));
    }

    void RegisterWorldTransform()
    {
        ComponentDrawerOptions opts;
        opts.ShowInAddMenu = false;

        ComponentDrawerRegistry::Register<WorldTransform>(
            "World Transform",
            [](Entity entity, WorldTransform& transform) {},
            std::move(opts));
    }
}
#else
namespace Luth::ComponentDrawers
{
    // Non-DEBUG builds get no-op register functions to satisfy the umbrella's
    // forward declarations.
    void RegisterID() {}
    void RegisterParent() {}
    void RegisterChildren() {}
    void RegisterWorldTransform() {}
}
#endif
