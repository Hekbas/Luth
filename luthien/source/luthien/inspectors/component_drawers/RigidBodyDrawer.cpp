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

    namespace
    {
        const char* kMotionStrings [] = { "Static", "Kinematic", "Dynamic" };
        const char* kQualityStrings[] = { "Discrete", "LinearCast" };

        // Patch the registry entry to fire on_update<RigidBody>; PhysicsSystem listens and queues
        // a body rebuild that drains at the next Update. Cheap (no field write) and idempotent
        // within a frame — the queue dedups, so multi-edit drags rebuild once per frame, not per
        // pixel.
        void Poke(Entity e) {
            e.GetScene()->Registry().patch<RigidBody>((entt::entity)e);
        }
    }

    void RegisterRigidBody()
    {
        ComponentDrawerOptions opts;
        opts.OnCopy = [](Entity e) {
            const auto& rb = e.GetComponent<RigidBody>();
            nlohmann::json j;
            j["motion"]         = (int)rb.motion;
            j["motionQuality"]  = (int)rb.motionQuality;
            j["layer"]          = rb.layer;
            j["isSensor"]       = rb.isSensor;
            j["startActive"]    = rb.startActive;
            j["mass"]           = rb.mass;
            j["gravityFactor"]  = rb.gravityFactor;
            j["linearDamping"]  = rb.linearDamping;
            j["angularDamping"] = rb.angularDamping;
            j["materialUUID"]   = rb.materialUUID.ToString();
            return j.dump();
        };
        opts.OnPaste = [](Entity e, const std::string& data) -> bool {
            try {
                auto j = nlohmann::json::parse(data);
                RigidBody newRb;
                newRb.motion         = (RigidBody::Motion)j.value("motion", 2);
                newRb.motionQuality  = (RigidBody::Quality)j.value("motionQuality", 0);
                newRb.layer          = (u8)j.value("layer", 1);
                newRb.isSensor       = j.value("isSensor", false);
                newRb.startActive    = j.value("startActive", true);
                newRb.mass           = j.value("mass", 0.0f);
                newRb.gravityFactor  = j.value("gravityFactor", 1.0f);
                newRb.linearDamping  = j.value("linearDamping", 0.05f);
                newRb.angularDamping = j.value("angularDamping", 0.05f);
                newRb.materialUUID   = UUID::FromString(j.value("materialUUID", ""));
                CommandHistory::Execute(std::make_unique<ComponentReplaceCommand<RigidBody>>(
                    "Paste RigidBody", e.GetScene(), (entt::entity)e, std::move(newRb)));
                return true;
            } catch (...) { return false; }
        };

        ComponentDrawerRegistry::Register<RigidBody>(
            "Rigid Body",
            [](Entity entity, RigidBody& rb) {
                if (UI::BeginProperties("RigidBodyProps")) {
                    Scene* scene = entity.GetScene();
                    entt::entity ent = (entt::entity)entity;

                    {
                        int current = (int)rb.motion;
                        auto state = UI::PropertyCombo("Motion", current, kMotionStrings, 3);
                        if (state.committed) {
                            auto oldVal = (RigidBody::Motion)UI::ConsumeItemPreEdit<int>(state.itemId);
                            rb.motion = (RigidBody::Motion)current;
                            CommandHistory::Execute(std::make_unique<ComponentPropertyCommand<RigidBody, RigidBody::Motion>>(
                                "Change Motion", scene, ent, &RigidBody::motion, oldVal, rb.motion));
                            Poke(entity);
                        }
                    }

                    {
                        int current = (int)rb.motionQuality;
                        auto state = UI::PropertyCombo("Motion Quality", current, kQualityStrings, 2);
                        if (state.committed) {
                            auto oldVal = (RigidBody::Quality)UI::ConsumeItemPreEdit<int>(state.itemId);
                            rb.motionQuality = (RigidBody::Quality)current;
                            CommandHistory::Execute(std::make_unique<ComponentPropertyCommand<RigidBody, RigidBody::Quality>>(
                                "Change Motion Quality", scene, ent, &RigidBody::motionQuality, oldVal, rb.motionQuality));
                            Poke(entity);
                        }
                    }

                    {
                        int current = (int)rb.layer;
                        auto state = UI::Property("Layer", current, 0, 255);
                        if (state.changed) { rb.layer = (u8)current; Poke(entity); }
                        if (state.committed) {
                            u8 oldVal = (u8)UI::ConsumeItemPreEdit<int>(state.itemId);
                            CommandHistory::Execute(std::make_unique<ComponentPropertyCommand<RigidBody, u8>>(
                                "Change Layer", scene, ent, &RigidBody::layer, oldVal, rb.layer));
                        }
                    }

                    {
                        auto state = UI::Property("Is Sensor", rb.isSensor);
                        if (state.changed) Poke(entity);
                        if (state.committed) {
                            bool oldVal = UI::ConsumeItemPreEdit<bool>(state.itemId);
                            CommandHistory::Execute(std::make_unique<ComponentPropertyCommand<RigidBody, bool>>(
                                "Change Is Sensor", scene, ent, &RigidBody::isSensor, oldVal, rb.isSensor));
                        }
                    }

                    {
                        // Authoring-only: only consulted when the body is created. No Poke — current
                        // body unaffected, next create picks up the new value.
                        auto state = UI::Property("Start Active", rb.startActive);
                        if (state.committed) {
                            bool oldVal = UI::ConsumeItemPreEdit<bool>(state.itemId);
                            CommandHistory::Execute(std::make_unique<ComponentPropertyCommand<RigidBody, bool>>(
                                "Change Start Active", scene, ent, &RigidBody::startActive, oldVal, rb.startActive));
                        }
                    }

                    {
                        auto state = UI::Property("Mass", rb.mass, 0.1f, 0.0f, 10000.0f);
                        if (state.changed) Poke(entity);
                        if (state.committed) {
                            f32 oldVal = UI::ConsumeItemPreEdit<float>(state.itemId);
                            CommandHistory::Execute(std::make_unique<ComponentPropertyCommand<RigidBody, float>>(
                                "Change Mass", scene, ent, &RigidBody::mass, oldVal, rb.mass));
                        }
                    }

                    {
                        auto state = UI::Property("Gravity Factor", rb.gravityFactor, 0.01f, -10.0f, 10.0f);
                        if (state.changed) Poke(entity);
                        if (state.committed) {
                            f32 oldVal = UI::ConsumeItemPreEdit<float>(state.itemId);
                            CommandHistory::Execute(std::make_unique<ComponentPropertyCommand<RigidBody, float>>(
                                "Change Gravity Factor", scene, ent, &RigidBody::gravityFactor, oldVal, rb.gravityFactor));
                        }
                    }

                    {
                        auto state = UI::Property("Linear Damping", rb.linearDamping, 0.01f, 0.0f, 10.0f);
                        if (state.changed) Poke(entity);
                        if (state.committed) {
                            f32 oldVal = UI::ConsumeItemPreEdit<float>(state.itemId);
                            CommandHistory::Execute(std::make_unique<ComponentPropertyCommand<RigidBody, float>>(
                                "Change Linear Damping", scene, ent, &RigidBody::linearDamping, oldVal, rb.linearDamping));
                        }
                    }

                    {
                        auto state = UI::Property("Angular Damping", rb.angularDamping, 0.01f, 0.0f, 10.0f);
                        if (state.changed) Poke(entity);
                        if (state.committed) {
                            f32 oldVal = UI::ConsumeItemPreEdit<float>(state.itemId);
                            CommandHistory::Execute(std::make_unique<ComponentPropertyCommand<RigidBody, float>>(
                                "Change Angular Damping", scene, ent, &RigidBody::angularDamping, oldVal, rb.angularDamping));
                        }
                    }

                    {
                        // Drag-drop a .physmat from the Project panel onto this slot. UUID::Invalid
                        // (slot empty) falls back to PhysicsMaterial::Default() at body create.
                        auto state = UI::PropertyAsset("Physics Material", rb.materialUUID, AssetType::PhysicsMaterial);
                        if (state.changed) {
                            UUID oldVal = UI::ConsumeItemPreEdit<UUID>(state.itemId);
                            CommandHistory::Execute(std::make_unique<ComponentPropertyCommand<RigidBody, UUID>>(
                                "Change Physics Material", scene, ent,
                                &RigidBody::materialUUID, oldVal, rb.materialUUID));
                            Poke(entity);
                        }
                    }

                    UI::EndProperties();
                }
            },
            std::move(opts));
    }
}
