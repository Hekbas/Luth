#include "lepch.h"
#include "luthien/inspectors/ComponentDrawerRegistry.h"
#include "luthien/inspectors/component_drawers/RegisterComponentDrawers.h"
#include "luthien/widgets/Widgets.h"
#include "luthien/commands/Commands.h"
#include "luthien/CommandHistory.h"
#include "luth/scene/Components.h"
#include "luth/resources/AssetManager.h"

#include <glm/gtc/quaternion.hpp>
#include <nlohmann/json.hpp>

namespace Luth::ComponentDrawers
{
    using namespace Component;

    namespace
    {
        const char* kColliderTypeStrings[] = { "Box", "Sphere", "Capsule", "Convex Hull", "Mesh" };

        // Reseat the union with sensible defaults for `c.type`. Members are trivially copyable so
        // overwriting the storage in-place is safe.
        void ResetUnionForType(Collider& c)
        {
            switch (c.type)
            {
                case Collider::Type::Box:
                    c.boxHalfExtents = Vec3(0.5f);
                    break;
                case Collider::Type::Sphere:
                    c.sphereRadius = 0.5f;
                    break;
                case Collider::Type::Capsule:
                    c.capsule.radius     = 0.5f;
                    c.capsule.halfHeight = 0.5f;
                    break;
                case Collider::Type::ConvexHullRef:
                case Collider::Type::MeshRef:
                    c.meshRef.modelHi   = 0;
                    c.meshRef.modelLo   = 0;
                    c.meshRef.meshIndex = 0;
                    break;
            }
        }

        void Poke(Entity e) {
            e.GetScene()->Registry().patch<Collider>((entt::entity)e);
        }
    }

    void RegisterCollider()
    {
        ComponentDrawerOptions opts;
        opts.OnCopy = [](Entity e) {
            const auto& c = e.GetComponent<Collider>();
            nlohmann::json j;
            j["type"]          = (int)c.type;
            j["localOffset"]   = { c.localOffset.x, c.localOffset.y, c.localOffset.z };
            j["localRotation"] = { c.localRotation.w, c.localRotation.x, c.localRotation.y, c.localRotation.z };
            switch (c.type)
            {
                case Collider::Type::Box:
                    j["halfExtents"] = { c.boxHalfExtents.x, c.boxHalfExtents.y, c.boxHalfExtents.z };
                    break;
                case Collider::Type::Sphere:
                    j["sphereRadius"] = c.sphereRadius;
                    break;
                case Collider::Type::Capsule:
                    j["capsuleRadius"]     = c.capsule.radius;
                    j["capsuleHalfHeight"] = c.capsule.halfHeight;
                    break;
                case Collider::Type::ConvexHullRef:
                case Collider::Type::MeshRef: {
                    UUID model(c.meshRef.modelHi, c.meshRef.modelLo);
                    j["modelUUID"] = model.ToString();
                    j["meshIndex"] = c.meshRef.meshIndex;
                    break;
                }
            }
            return j.dump();
        };
        opts.OnPaste = [](Entity e, const std::string& data) -> bool {
            try {
                auto j = nlohmann::json::parse(data);
                Collider newC;
                newC.type = (Collider::Type)j.value("type", 0);
                if (j.contains("localOffset") && j["localOffset"].is_array() && j["localOffset"].size() >= 3)
                    newC.localOffset = { j["localOffset"][0], j["localOffset"][1], j["localOffset"][2] };
                if (j.contains("localRotation") && j["localRotation"].is_array() && j["localRotation"].size() >= 4)
                    newC.localRotation = Quat(j["localRotation"][0], j["localRotation"][1],
                                              j["localRotation"][2], j["localRotation"][3]);
                switch (newC.type)
                {
                    case Collider::Type::Box:
                        if (j.contains("halfExtents") && j["halfExtents"].is_array() && j["halfExtents"].size() >= 3)
                            newC.boxHalfExtents = { j["halfExtents"][0], j["halfExtents"][1], j["halfExtents"][2] };
                        else
                            newC.boxHalfExtents = Vec3(0.5f);
                        break;
                    case Collider::Type::Sphere:
                        newC.sphereRadius = j.value("sphereRadius", 0.5f);
                        break;
                    case Collider::Type::Capsule:
                        newC.capsule.radius     = j.value("capsuleRadius", 0.5f);
                        newC.capsule.halfHeight = j.value("capsuleHalfHeight", 0.5f);
                        break;
                    case Collider::Type::ConvexHullRef:
                    case Collider::Type::MeshRef: {
                        UUID model = UUID::FromString(j.value("modelUUID", ""));
                        newC.meshRef.modelHi   = model.GetHalf0();
                        newC.meshRef.modelLo   = model.GetHalf1();
                        newC.meshRef.meshIndex = j.value("meshIndex", 0u);
                        break;
                    }
                }
                CommandHistory::Execute(std::make_unique<ComponentReplaceCommand<Collider>>(
                    "Paste Collider", e.GetScene(), (entt::entity)e, std::move(newC)));
                return true;
            } catch (...) { return false; }
        };

        ComponentDrawerRegistry::Register<Collider>(
            "Collider",
            [](Entity entity, Collider& collider) {
                if (UI::BeginProperties("ColliderProps")) {
                    Scene* scene = entity.GetScene();
                    entt::entity ent = (entt::entity)entity;

                    // Type combo. Type change reseats the union — capture whole-component snapshot
                    // so undo restores both the type and the previous union content.
                    {
                        int current = (int)collider.type;
                        auto state = UI::PropertyCombo("Type", current, kColliderTypeStrings, 5);
                        if (state.committed) {
                            int oldType = UI::ConsumeItemPreEdit<int>(state.itemId);
                            if (oldType != current) {
                                Collider oldC = collider;
                                Collider newC = collider;
                                newC.type = (Collider::Type)current;
                                ResetUnionForType(newC);
                                CommandHistory::Execute(std::make_unique<ComponentSnapshotCommand<Collider>>(
                                    "Change Collider Type", scene, ent, std::move(oldC), std::move(newC)));
                                Poke(entity);
                            }
                        }
                    }

                    {
                        auto state = UI::Property("Local Offset", collider.localOffset);
                        if (state.changed) Poke(entity);
                        if (state.committed) {
                            Vec3 oldVal = UI::ConsumeItemPreEdit<Vec3>(state.itemId);
                            CommandHistory::Execute(std::make_unique<ComponentPropertyCommand<Collider, Vec3>>(
                                "Change Collider Offset", scene, ent,
                                &Collider::localOffset, oldVal, collider.localOffset));
                        }
                    }

                    // Local Rotation: Quat under the hood, edited as Euler degrees for parity with
                    // Transform's rotation editor. Pre-edit cache is keyed on the Vec3 widget id, so
                    // both the live edit and the undo snapshot work in Euler space.
                    {
                        Vec3 eulerDeg = glm::eulerAngles(collider.localRotation) * Math::RadToDeg<f32>;
                        auto state = UI::Property("Local Rotation", eulerDeg);
                        if (state.changed) {
                            collider.localRotation = Quat(Math::Radians(eulerDeg));
                            Poke(entity);
                        }
                        if (state.committed) {
                            Vec3 oldEuler = UI::ConsumeItemPreEdit<Vec3>(state.itemId);
                            Quat oldQuat  = Quat(Math::Radians(oldEuler));
                            CommandHistory::Execute(std::make_unique<ComponentPropertyCommand<Collider, Quat>>(
                                "Change Collider Rotation", scene, ent,
                                &Collider::localRotation, oldQuat, collider.localRotation));
                        }
                    }

                    // Type-specific fields. Whole-component snapshot — anonymous-union members have
                    // no usable pointer-to-member, and ComponentReplaceCommand would mis-capture
                    // because the live component was already mutated by the widget.
                    switch (collider.type)
                    {
                        case Collider::Type::Box: {
                            auto state = UI::Property("Half Extents", collider.boxHalfExtents, 0.05f, 0.5f);
                            if (state.changed) Poke(entity);
                            if (state.committed) {
                                Collider oldC = collider;
                                oldC.boxHalfExtents = UI::ConsumeItemPreEdit<Vec3>(state.itemId);
                                Collider newC = collider;
                                CommandHistory::Execute(std::make_unique<ComponentSnapshotCommand<Collider>>(
                                    "Change Box Extents", scene, ent, std::move(oldC), std::move(newC)));
                            }
                            break;
                        }
                        case Collider::Type::Sphere: {
                            auto state = UI::Property("Radius", collider.sphereRadius, 0.05f, 0.01f, 1000.0f);
                            if (state.changed) Poke(entity);
                            if (state.committed) {
                                Collider oldC = collider;
                                oldC.sphereRadius = UI::ConsumeItemPreEdit<float>(state.itemId);
                                Collider newC = collider;
                                CommandHistory::Execute(std::make_unique<ComponentSnapshotCommand<Collider>>(
                                    "Change Sphere Radius", scene, ent, std::move(oldC), std::move(newC)));
                            }
                            break;
                        }
                        case Collider::Type::Capsule: {
                            {
                                auto state = UI::Property("Radius", collider.capsule.radius, 0.05f, 0.01f, 1000.0f);
                                if (state.changed) Poke(entity);
                                if (state.committed) {
                                    Collider oldC = collider;
                                    oldC.capsule.radius = UI::ConsumeItemPreEdit<float>(state.itemId);
                                    Collider newC = collider;
                                    CommandHistory::Execute(std::make_unique<ComponentSnapshotCommand<Collider>>(
                                        "Change Capsule Radius", scene, ent, std::move(oldC), std::move(newC)));
                                }
                            }
                            {
                                auto state = UI::Property("Half Height", collider.capsule.halfHeight, 0.05f, 0.01f, 1000.0f);
                                if (state.changed) Poke(entity);
                                if (state.committed) {
                                    Collider oldC = collider;
                                    oldC.capsule.halfHeight = UI::ConsumeItemPreEdit<float>(state.itemId);
                                    Collider newC = collider;
                                    CommandHistory::Execute(std::make_unique<ComponentSnapshotCommand<Collider>>(
                                        "Change Capsule Half Height", scene, ent, std::move(oldC), std::move(newC)));
                                }
                            }
                            break;
                        }
                        case Collider::Type::ConvexHullRef:
                        case Collider::Type::MeshRef: {
                            UUID currentModel(collider.meshRef.modelHi, collider.meshRef.modelLo);
                            {
                                auto state = UI::PropertyAsset("Model", currentModel, AssetType::Model);
                                if (state.changed) {
                                    collider.meshRef.modelHi = currentModel.GetHalf0();
                                    collider.meshRef.modelLo = currentModel.GetHalf1();
                                    Poke(entity);
                                }
                                if (state.committed) {
                                    UUID oldModel = UI::ConsumeItemPreEdit<UUID>(state.itemId);
                                    Collider oldC = collider;
                                    oldC.meshRef.modelHi = oldModel.GetHalf0();
                                    oldC.meshRef.modelLo = oldModel.GetHalf1();
                                    Collider newC = collider;
                                    CommandHistory::Execute(std::make_unique<ComponentSnapshotCommand<Collider>>(
                                        "Change Collider Model", scene, ent, std::move(oldC), std::move(newC)));
                                }
                            }
                            {
                                int meshIdx = (int)collider.meshRef.meshIndex;
                                auto state = UI::Property("Mesh Index", meshIdx, 0, 64);
                                if (state.changed) {
                                    collider.meshRef.meshIndex = (u32)meshIdx;
                                    Poke(entity);
                                }
                                if (state.committed) {
                                    Collider oldC = collider;
                                    oldC.meshRef.meshIndex = (u32)UI::ConsumeItemPreEdit<int>(state.itemId);
                                    Collider newC = collider;
                                    CommandHistory::Execute(std::make_unique<ComponentSnapshotCommand<Collider>>(
                                        "Change Mesh Index", scene, ent, std::move(oldC), std::move(newC)));
                                }
                            }
                            break;
                        }
                    }

                    UI::EndProperties();
                }
            },
            std::move(opts));
    }
}
