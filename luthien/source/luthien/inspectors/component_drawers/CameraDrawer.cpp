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

    void RegisterCamera()
    {
        ComponentDrawerOptions opts;
        opts.OnCopy = [](Entity e) {
            const auto& c = e.GetComponent<Camera>();
            nlohmann::json j;
            j["projection"]       = (int)c.Projection;
            j["verticalFOV"]      = c.VerticalFOV;
            j["nearClip"]         = c.NearClip;
            j["farClip"]          = c.FarClip;
            j["orthographicSize"] = c.OrthographicSize;
            j["orthographicNear"] = c.OrthographicNear;
            j["orthographicFar"]  = c.OrthographicFar;
            j["aspectRatio"]      = c.AspectRatio;
            return j.dump();
        };
        opts.OnPaste = [](Entity e, const std::string& data) -> bool {
            try {
                auto j = nlohmann::json::parse(data);
                Camera newC = e.GetComponent<Camera>();
                newC.Projection       = (Camera::ProjectionType)j.value("projection", 0);
                newC.VerticalFOV      = j.value("verticalFOV", 45.0f);
                newC.NearClip         = j.value("nearClip", 0.01f);
                newC.FarClip          = j.value("farClip", 1000.0f);
                newC.OrthographicSize = j.value("orthographicSize", 10.0f);
                newC.OrthographicNear = j.value("orthographicNear", -1.0f);
                newC.OrthographicFar  = j.value("orthographicFar", 1.0f);
                newC.AspectRatio      = j.value("aspectRatio", 16.0f / 9.0f);
                newC.IsDirty = true;
                CommandHistory::Execute(std::make_unique<ComponentReplaceCommand<Camera>>(
                    "Paste Camera", e.GetScene(), (entt::entity)e, std::move(newC)));
                return true;
            } catch (...) { return false; }
        };

        ComponentDrawerRegistry::Register<Camera>(
            "Camera",
            [](Entity e, Camera& camera) {
                if (UI::BeginProperties("CameraProps")) {
                    Scene* scene = e.GetScene();
                    entt::entity ent = (entt::entity)e;

                    const char* projectionTypeStrings[] = { "Perspective", "Orthographic" };
                    int currentProj = (int)camera.Projection;
                    {
                        auto state = UI::PropertyCombo("Projection", currentProj, projectionTypeStrings, 2);
                        if (state.committed) {
                            auto oldProj = (Camera::ProjectionType)UI::ConsumeItemPreEdit<int>(state.itemId);
                            camera.Projection = (Camera::ProjectionType)currentProj;
                            CommandHistory::Execute(std::make_unique<ComponentPropertyCommand<Camera, Camera::ProjectionType>>(
                                "Change Projection", scene, ent,
                                &Camera::Projection, oldProj, camera.Projection));
                            camera.IsDirty = true;
                        }
                    }

                    bool changed = false;
                    if (camera.Projection == Camera::ProjectionType::Perspective) {
                        {
                            auto state = UI::Property("FOV", camera.VerticalFOV, 0.1f, 1.0f, 180.0f);
                            if (state.changed) changed = true;
                            if (state.committed)
                                EXEC_COMPONENT_PROP("Change FOV", scene, ent, Camera, VerticalFOV,
                                    UI::ConsumeItemPreEdit<float>(state.itemId), camera.VerticalFOV);
                        }
                        {
                            auto state = UI::Property("Near", camera.NearClip, 0.01f, 0.01f, camera.FarClip);
                            if (state.changed) changed = true;
                            if (state.committed)
                                EXEC_COMPONENT_PROP("Change Near Clip", scene, ent, Camera, NearClip,
                                    UI::ConsumeItemPreEdit<float>(state.itemId), camera.NearClip);
                        }
                        {
                            auto state = UI::Property("Far", camera.FarClip, 0.1f, camera.NearClip, 10000.0f);
                            if (state.changed) changed = true;
                            if (state.committed)
                                CommandHistory::Execute(std::make_unique<ComponentPropertyCommand<Camera, float>>(
                                    "Change Far Clip", scene, ent, &Camera::FarClip,
                                    UI::ConsumeItemPreEdit<float>(state.itemId), camera.FarClip));
                        }
                    }
                    else {
                        {
                            auto state = UI::Property("Size", camera.OrthographicSize, 0.1f, 0.1f, 100.0f);
                            if (state.changed) changed = true;
                            if (state.committed)
                                CommandHistory::Execute(std::make_unique<ComponentPropertyCommand<Camera, float>>(
                                    "Change Ortho Size", scene, ent, &Camera::OrthographicSize,
                                    UI::ConsumeItemPreEdit<float>(state.itemId), camera.OrthographicSize));
                        }
                        {
                            auto state = UI::Property("Near", camera.OrthographicNear, 0.01f);
                            if (state.changed) changed = true;
                            if (state.committed)
                                CommandHistory::Execute(std::make_unique<ComponentPropertyCommand<Camera, float>>(
                                    "Change Ortho Near", scene, ent, &Camera::OrthographicNear,
                                    UI::ConsumeItemPreEdit<float>(state.itemId), camera.OrthographicNear));
                        }
                        {
                            auto state = UI::Property("Far", camera.OrthographicFar, 0.01f);
                            if (state.changed) changed = true;
                            if (state.committed)
                                CommandHistory::Execute(std::make_unique<ComponentPropertyCommand<Camera, float>>(
                                    "Change Ortho Far", scene, ent, &Camera::OrthographicFar,
                                    UI::ConsumeItemPreEdit<float>(state.itemId), camera.OrthographicFar));
                        }
                    }

                    {
                        auto state = UI::Property("Aspect", camera.AspectRatio, 0.01f, 0.1f, 10.0f);
                        if (state.changed) changed = true;
                        if (state.committed)
                            CommandHistory::Execute(std::make_unique<ComponentPropertyCommand<Camera, float>>(
                                "Change Aspect", scene, ent, &Camera::AspectRatio,
                                UI::ConsumeItemPreEdit<float>(state.itemId), camera.AspectRatio));
                    }

                    if (changed) camera.IsDirty = true;
                    UI::EndProperties();
                }
            },
            std::move(opts));
    }
}
