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
        // Patch fires on_update<CharacterController>; PhysicsSystem queues a rebuild that drains at
        // next Update. Fast path applies fingerprint-stable tunables in place — only structural fields
        // (capsule shape, padding, predictive distance) actually tear down and recreate the JPH object.
        void Poke(Entity e) {
            e.GetScene()->Registry().patch<CharacterController>((entt::entity)e);
        }

        const char* GroundStateLabel(GroundState s)
        {
            switch (s) {
                case GroundState::OnGround:      return "On Ground";
                case GroundState::OnSteepGround: return "On Steep Ground";
                case GroundState::NotSupported:  return "Not Supported";
                case GroundState::InAir:         return "In Air";
            }
            return "?";
        }
    }

    void RegisterCharacterController()
    {
        ComponentDrawerOptions opts;

        // Copy/paste — authoring fields only. Per-frame inputs (desiredVelocity, jumpQueued) and
        // read-back (groundState, currentVelocity) are excluded.
        opts.OnCopy = [](Entity e) {
            const auto& cc = e.GetComponent<CharacterController>();
            nlohmann::json j;
            j["maxSlopeAngleDeg"]          = cc.maxSlopeAngleDeg;
            j["mass"]                      = cc.mass;
            j["maxStrength"]               = cc.maxStrength;
            j["characterPadding"]          = cc.characterPadding;
            j["predictiveContactDistance"] = cc.predictiveContactDistance;
            j["penetrationRecoverySpeed"]  = cc.penetrationRecoverySpeed;
            j["layer"]                     = cc.layer;
            j["gravityFactor"]             = cc.gravityFactor;
            j["moveSpeed"]                 = cc.moveSpeed;
            j["jumpSpeed"]                 = cc.jumpSpeed;
            return j.dump();
        };
        opts.OnPaste = [](Entity e, const std::string& data) -> bool {
            try {
                auto j = nlohmann::json::parse(data);
                CharacterController newCc;
                newCc.maxSlopeAngleDeg          = j.value("maxSlopeAngleDeg",          45.0f);
                newCc.mass                      = j.value("mass",                      70.0f);
                newCc.maxStrength               = j.value("maxStrength",               100.0f);
                newCc.characterPadding          = j.value("characterPadding",          0.02f);
                newCc.predictiveContactDistance = j.value("predictiveContactDistance", 0.1f);
                newCc.penetrationRecoverySpeed  = j.value("penetrationRecoverySpeed",  1.0f);
                newCc.layer                     = (u8)j.value("layer",                 1);
                newCc.gravityFactor             = j.value("gravityFactor",             1.0f);
                newCc.moveSpeed                 = j.value("moveSpeed",                 5.0f);
                newCc.jumpSpeed                 = j.value("jumpSpeed",                 6.0f);
                CommandHistory::Execute(std::make_unique<ComponentReplaceCommand<CharacterController>>(
                    "Paste CharacterController", e.GetScene(), (entt::entity)e, std::move(newCc)));
                return true;
            } catch (...) { return false; }
        };

        // Custom OnAdd: if the entity has no Collider, also add a default Capsule Collider in the same
        // CompoundCommand. Collider goes FIRST so EnTT's synchronous on_construct sees a valid pair when
        // CharacterController's signal fires — otherwise the first drain hits Failed (no Collider) and
        // logs the non-capsule warning before it self-heals next frame.
        opts.OnAdd = [](Entity e) {
            Scene* scene = e.GetScene();
            entt::entity ent = (entt::entity)e;
            if (e.HasComponent<Collider>()) {
                CommandHistory::Execute(std::make_unique<ComponentAddCommand<CharacterController>>(
                    "Add Character Controller", scene, ent));
            } else {
                Collider initCol;
                initCol.type = Collider::Type::Capsule;
                initCol.capsule.radius     = 0.4f;
                initCol.capsule.halfHeight = 0.9f;

                std::vector<std::unique_ptr<ICommand>> cmds;
                cmds.push_back(std::make_unique<ComponentAddCommand<Collider>>(
                    "Add Capsule Collider", scene, ent, std::move(initCol)));
                cmds.push_back(std::make_unique<ComponentAddCommand<CharacterController>>(
                    "Add Character Controller", scene, ent));
                CommandHistory::Execute(std::make_unique<CompoundCommand>(
                    "Add Character Controller", std::move(cmds)));
            }
        };

        ComponentDrawerRegistry::Register<CharacterController>(
            "Character Controller",
            [](Entity entity, CharacterController& cc) {
                if (UI::BeginProperties("CharacterControllerProps")) {
                    Scene* scene = entity.GetScene();
                    entt::entity ent = (entt::entity)entity;

                    {
                        auto state = UI::Property("Max Slope (deg)", cc.maxSlopeAngleDeg, 0.5f, 0.0f, 90.0f);
                        if (state.changed) Poke(entity);
                        if (state.committed) {
                            f32 oldVal = UI::ConsumeItemPreEdit<float>(state.itemId);
                            CommandHistory::Execute(std::make_unique<ComponentPropertyCommand<CharacterController, float>>(
                                "Change Max Slope", scene, ent, &CharacterController::maxSlopeAngleDeg, oldVal, cc.maxSlopeAngleDeg));
                        }
                    }

                    {
                        auto state = UI::Property("Mass", cc.mass, 0.5f, 0.1f, 10000.0f);
                        if (state.changed) Poke(entity);
                        if (state.committed) {
                            f32 oldVal = UI::ConsumeItemPreEdit<float>(state.itemId);
                            CommandHistory::Execute(std::make_unique<ComponentPropertyCommand<CharacterController, float>>(
                                "Change Mass", scene, ent, &CharacterController::mass, oldVal, cc.mass));
                        }
                    }

                    {
                        auto state = UI::Property("Max Strength", cc.maxStrength, 0.5f, 0.0f, 10000.0f);
                        if (state.changed) Poke(entity);
                        if (state.committed) {
                            f32 oldVal = UI::ConsumeItemPreEdit<float>(state.itemId);
                            CommandHistory::Execute(std::make_unique<ComponentPropertyCommand<CharacterController, float>>(
                                "Change Max Strength", scene, ent, &CharacterController::maxStrength, oldVal, cc.maxStrength));
                        }
                    }

                    {
                        // Structural — change forces tear-down + rebuild (no JPH setter for padding).
                        auto state = UI::Property("Character Padding", cc.characterPadding, 0.001f, 0.001f, 0.5f);
                        if (state.changed) Poke(entity);
                        if (state.committed) {
                            f32 oldVal = UI::ConsumeItemPreEdit<float>(state.itemId);
                            CommandHistory::Execute(std::make_unique<ComponentPropertyCommand<CharacterController, float>>(
                                "Change Character Padding", scene, ent, &CharacterController::characterPadding, oldVal, cc.characterPadding));
                        }
                    }

                    {
                        // Structural — same reason as padding (no JPH setter).
                        auto state = UI::Property("Predictive Contact", cc.predictiveContactDistance, 0.005f, 0.01f, 1.0f);
                        if (state.changed) Poke(entity);
                        if (state.committed) {
                            f32 oldVal = UI::ConsumeItemPreEdit<float>(state.itemId);
                            CommandHistory::Execute(std::make_unique<ComponentPropertyCommand<CharacterController, float>>(
                                "Change Predictive Contact", scene, ent, &CharacterController::predictiveContactDistance, oldVal, cc.predictiveContactDistance));
                        }
                    }

                    {
                        auto state = UI::Property("Penetration Recovery", cc.penetrationRecoverySpeed, 0.01f, 0.0f, 1.0f);
                        if (state.changed) Poke(entity);
                        if (state.committed) {
                            f32 oldVal = UI::ConsumeItemPreEdit<float>(state.itemId);
                            CommandHistory::Execute(std::make_unique<ComponentPropertyCommand<CharacterController, float>>(
                                "Change Penetration Recovery", scene, ent, &CharacterController::penetrationRecoverySpeed, oldVal, cc.penetrationRecoverySpeed));
                        }
                    }

                    {
                        int current = (int)cc.layer;
                        auto state = UI::Property("Layer", current, 0, 255);
                        if (state.changed) { cc.layer = (u8)current; Poke(entity); }
                        if (state.committed) {
                            u8 oldVal = (u8)UI::ConsumeItemPreEdit<int>(state.itemId);
                            CommandHistory::Execute(std::make_unique<ComponentPropertyCommand<CharacterController, u8>>(
                                "Change Layer", scene, ent, &CharacterController::layer, oldVal, cc.layer));
                        }
                    }

                    {
                        // Always-live: read by UpdateCharacters each substep. No Poke needed.
                        auto state = UI::Property("Gravity Factor", cc.gravityFactor, 0.01f, -10.0f, 10.0f);
                        if (state.committed) {
                            f32 oldVal = UI::ConsumeItemPreEdit<float>(state.itemId);
                            CommandHistory::Execute(std::make_unique<ComponentPropertyCommand<CharacterController, float>>(
                                "Change Gravity Factor", scene, ent, &CharacterController::gravityFactor, oldVal, cc.gravityFactor));
                        }
                    }

                    {
                        // Consumed by PlayerControllerSystem stub. No Poke (no JPH state to refresh).
                        auto state = UI::Property("Move Speed", cc.moveSpeed, 0.1f, 0.0f, 50.0f);
                        if (state.committed) {
                            f32 oldVal = UI::ConsumeItemPreEdit<float>(state.itemId);
                            CommandHistory::Execute(std::make_unique<ComponentPropertyCommand<CharacterController, float>>(
                                "Change Move Speed", scene, ent, &CharacterController::moveSpeed, oldVal, cc.moveSpeed));
                        }
                    }

                    {
                        auto state = UI::Property("Jump Speed", cc.jumpSpeed, 0.1f, 0.0f, 50.0f);
                        if (state.committed) {
                            f32 oldVal = UI::ConsumeItemPreEdit<float>(state.itemId);
                            CommandHistory::Execute(std::make_unique<ComponentPropertyCommand<CharacterController, float>>(
                                "Change Jump Speed", scene, ent, &CharacterController::jumpSpeed, oldVal, cc.jumpSpeed));
                        }
                    }

                    // Read-back rows. Disabled controls — visual only, no command emit.
                    ImGui::BeginDisabled();
                    {
                        Vec3 vel = cc.currentVelocity;
                        UI::Property("Velocity", vel);
                        // GroundState as a string in a disabled int property would be misleading; render
                        // as inline label-value text instead.
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted("Ground");
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted(GroundStateLabel(cc.groundState));
                    }
                    ImGui::EndDisabled();

                    UI::EndProperties();
                }
            },
            std::move(opts));
    }
}
