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

    void RegisterCamera()
    {
        ComponentDrawerRegistry::RegisterSimple<Camera>(
            "Camera",
            [](Entity e, Camera& camera) {
                if (UI::BeginProperties("CameraProps")) {
                    Scene* scene = e.GetScene();
                    entt::entity ent = (entt::entity)e;

                    const char* projectionTypeStrings[] = { "Perspective", "Orthographic" };
                    int currentProj = (int)camera.Projection;
                    if (UI::PropertyCombo("Projection", currentProj, projectionTypeStrings, 2)) {
                        auto oldProj = camera.Projection;
                        camera.Projection = (Camera::ProjectionType)currentProj;
                        CommandHistory::Execute(std::make_unique<ComponentPropertyCommand<Camera, Camera::ProjectionType>>(
                            "Change Projection", scene, ent,
                            &Camera::Projection, oldProj, camera.Projection));
                        camera.IsDirty = true;
                    }

                    auto snapFOV = camera.VerticalFOV;
                    auto snapNear = camera.NearClip;
                    auto snapFar = camera.FarClip;
                    auto snapOrthoSize = camera.OrthographicSize;
                    auto snapOrthoNear = camera.OrthographicNear;
                    auto snapOrthoFar = camera.OrthographicFar;
                    auto snapAspect = camera.AspectRatio;

                    bool changed = false;
                    if (camera.Projection == Camera::ProjectionType::Perspective) {
                        if (UI::Property("FOV", camera.VerticalFOV, 0.1f, 1.0f, 180.0f)) {
                            EXEC_COMPONENT_PROP("Change FOV", scene, ent, Camera, VerticalFOV, snapFOV, camera.VerticalFOV);
                            changed = true;
                        }
                        if (UI::Property("Near", camera.NearClip, 0.01f, 0.01f, camera.FarClip)) {
                            EXEC_COMPONENT_PROP("Change Near Clip", scene, ent, Camera, NearClip, snapNear, camera.NearClip);
                            changed = true;
                        }
                        if (UI::Property("Far", camera.FarClip, 0.1f, camera.NearClip, 10000.0f)) {
                            CommandHistory::Execute(std::make_unique<ComponentPropertyCommand<Camera, float>>(
                                "Change Far Clip", scene, ent, &Camera::FarClip, snapFar, camera.FarClip));
                            changed = true;
                        }
                    }
                    else {
                        if (UI::Property("Size", camera.OrthographicSize, 0.1f, 0.1f, 100.0f)) {
                            CommandHistory::Execute(std::make_unique<ComponentPropertyCommand<Camera, float>>(
                                "Change Ortho Size", scene, ent, &Camera::OrthographicSize, snapOrthoSize, camera.OrthographicSize));
                            changed = true;
                        }
                        if (UI::Property("Near", camera.OrthographicNear, 0.01f)) {
                            CommandHistory::Execute(std::make_unique<ComponentPropertyCommand<Camera, float>>(
                                "Change Ortho Near", scene, ent, &Camera::OrthographicNear, snapOrthoNear, camera.OrthographicNear));
                            changed = true;
                        }
                        if (UI::Property("Far", camera.OrthographicFar, 0.01f)) {
                            CommandHistory::Execute(std::make_unique<ComponentPropertyCommand<Camera, float>>(
                                "Change Ortho Far", scene, ent, &Camera::OrthographicFar, snapOrthoFar, camera.OrthographicFar));
                            changed = true;
                        }
                    }

                    if (UI::Property("Aspect", camera.AspectRatio, 0.01f, 0.1f, 10.0f)) {
                        CommandHistory::Execute(std::make_unique<ComponentPropertyCommand<Camera, float>>(
                            "Change Aspect", scene, ent, &Camera::AspectRatio, snapAspect, camera.AspectRatio));
                        changed = true;
                    }

                    if (changed) camera.IsDirty = true;
                    UI::EndProperties();
                }
            });
    }
}
