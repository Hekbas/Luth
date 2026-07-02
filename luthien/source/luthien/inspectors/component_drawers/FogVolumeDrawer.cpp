#include "lepch.h"
#include "luthien/inspectors/ComponentDrawerRegistry.h"
#include "luthien/inspectors/component_drawers/RegisterComponentDrawers.h"
#include "luthien/widgets/Widgets.h"
#include "luthien/commands/Commands.h"
#include "luthien/CommandHistory.h"
#include "luth/scene/Components.h"

#include <glm/gtc/quaternion.hpp>
#include <nlohmann/json.hpp>

namespace Luth::ComponentDrawers
{
    using namespace Component;

    namespace
    {
        const char* kFogVolumeTypeStrings[] = { "Box", "Sphere" };

        // Reseat the union with sensible defaults for `v.type`. Members are trivially copyable, so
        // overwriting in-place is safe. Mirrors ColliderDrawer's ResetUnionForType.
        void ResetUnionForType(FogVolume& v)
        {
            switch (v.type)
            {
                case FogVolume::Type::Box:    v.halfExtents = Vec3(2.0f); break;
                case FogVolume::Type::Sphere: v.radius      = 2.0f;       break;
            }
        }

        void Poke(Entity e) {
            e.GetScene()->Registry().patch<FogVolume>((entt::entity)e);
        }
    }

    void RegisterFogVolume()
    {
        ComponentDrawerOptions opts;
        opts.OnCopy = [](Entity e) {
            const auto& v = e.GetComponent<FogVolume>();
            nlohmann::json j;
            j["type"]           = (int)v.type;
            j["localOffset"]    = { v.localOffset.x, v.localOffset.y, v.localOffset.z };
            j["localRotation"]  = { v.localRotation.w, v.localRotation.x, v.localRotation.y, v.localRotation.z };
            switch (v.type)
            {
                case FogVolume::Type::Box:
                    j["halfExtents"] = { v.halfExtents.x, v.halfExtents.y, v.halfExtents.z };
                    break;
                case FogVolume::Type::Sphere:
                    j["radius"] = v.radius;
                    break;
            }
            j["color"]          = { v.color.x, v.color.y, v.color.z };
            j["density"]        = v.density;
            j["falloffStart"]   = v.falloffStart;
            j["falloffEnd"]     = v.falloffEnd;
            j["affectsAmbient"] = v.affectsAmbient;
            return j.dump();
        };
        opts.OnPaste = [](Entity e, const std::string& data) -> bool {
            try {
                auto j = nlohmann::json::parse(data);
                FogVolume newV;
                newV.type = (FogVolume::Type)j.value("type", 0);
                if (j.contains("localOffset") && j["localOffset"].is_array() && j["localOffset"].size() >= 3)
                    newV.localOffset = { j["localOffset"][0], j["localOffset"][1], j["localOffset"][2] };
                if (j.contains("localRotation") && j["localRotation"].is_array() && j["localRotation"].size() >= 4)
                    newV.localRotation = Quat(j["localRotation"][0], j["localRotation"][1],
                                              j["localRotation"][2], j["localRotation"][3]);
                switch (newV.type)
                {
                    case FogVolume::Type::Box:
                        if (j.contains("halfExtents") && j["halfExtents"].is_array() && j["halfExtents"].size() >= 3)
                            newV.halfExtents = { j["halfExtents"][0], j["halfExtents"][1], j["halfExtents"][2] };
                        else
                            newV.halfExtents = Vec3(2.0f);
                        break;
                    case FogVolume::Type::Sphere:
                        newV.radius = j.value("radius", 2.0f);
                        break;
                }
                if (j.contains("color") && j["color"].is_array() && j["color"].size() >= 3)
                    newV.color = { j["color"][0], j["color"][1], j["color"][2] };
                newV.density        = j.value("density",        0.1f);
                newV.falloffStart   = j.value("falloffStart",   0.0f);
                newV.falloffEnd     = j.value("falloffEnd",     1.0f);
                newV.affectsAmbient = j.value("affectsAmbient", true);
                CommandHistory::Execute(std::make_unique<ComponentReplaceCommand<FogVolume>>(
                    "Paste FogVolume", e.GetScene(), (entt::entity)e, std::move(newV)));
                return true;
            } catch (...) { return false; }
        };

        ComponentDrawerRegistry::Register<FogVolume>(
            "Fog Volume",
            [](Entity entity, FogVolume& v) {
                if (UI::BeginProperties("FogVolumeProps")) {
                    Scene* scene = entity.GetScene();
                    entt::entity ent = (entt::entity)entity;

                    // Type combo. Change reseats the union; whole-component snapshot so undo
                    // restores both the type and the previous union content.
                    {
                        int current = (int)v.type;
                        auto state = UI::PropertyCombo("Type", current, kFogVolumeTypeStrings, 2);
                        if (state.committed) {
                            int oldType = UI::ConsumeItemPreEdit<int>(state.itemId);
                            if (oldType != current) {
                                FogVolume oldV = v;
                                FogVolume newV = v;
                                newV.type = (FogVolume::Type)current;
                                ResetUnionForType(newV);
                                CommandHistory::Execute(std::make_unique<ComponentSnapshotCommand<FogVolume>>(
                                    "Change FogVolume Type", scene, ent, std::move(oldV), std::move(newV)));
                                Poke(entity);
                            }
                        }
                    }

                    {
                        auto state = UI::Property("Local Offset", v.localOffset);
                        if (state.changed) Poke(entity);
                        if (state.committed) {
                            Vec3 oldVal = UI::ConsumeItemPreEdit<Vec3>(state.itemId);
                            CommandHistory::Execute(std::make_unique<ComponentPropertyCommand<FogVolume, Vec3>>(
                                "Change FogVolume Offset", scene, ent,
                                &FogVolume::localOffset, oldVal, v.localOffset));
                        }
                    }

                    {
                        Vec3 eulerDeg = glm::eulerAngles(v.localRotation) * Math::RadToDeg<f32>;
                        auto state = UI::Property("Local Rotation", eulerDeg);
                        if (state.changed) {
                            v.localRotation = Quat(Math::Radians(eulerDeg));
                            Poke(entity);
                        }
                        if (state.committed) {
                            Vec3 oldEuler = UI::ConsumeItemPreEdit<Vec3>(state.itemId);
                            Quat oldQuat  = Quat(Math::Radians(oldEuler));
                            CommandHistory::Execute(std::make_unique<ComponentPropertyCommand<FogVolume, Quat>>(
                                "Change FogVolume Rotation", scene, ent,
                                &FogVolume::localRotation, oldQuat, v.localRotation));
                        }
                    }

                    // Union members: whole-component snapshot (anonymous-union members have no
                    // usable pointer-to-member).
                    switch (v.type)
                    {
                        case FogVolume::Type::Box: {
                            auto state = UI::Property("Half Extents", v.halfExtents, 0.05f, 2.0f);
                            if (state.changed) Poke(entity);
                            if (state.committed) {
                                FogVolume oldV = v;
                                oldV.halfExtents = UI::ConsumeItemPreEdit<Vec3>(state.itemId);
                                FogVolume newV = v;
                                CommandHistory::Execute(std::make_unique<ComponentSnapshotCommand<FogVolume>>(
                                    "Change FogVolume Extents", scene, ent, std::move(oldV), std::move(newV)));
                            }
                            break;
                        }
                        case FogVolume::Type::Sphere: {
                            auto state = UI::Property("Radius", v.radius, 0.05f, 0.01f, 1000.0f);
                            if (state.changed) Poke(entity);
                            if (state.committed) {
                                FogVolume oldV = v;
                                oldV.radius = UI::ConsumeItemPreEdit<float>(state.itemId);
                                FogVolume newV = v;
                                CommandHistory::Execute(std::make_unique<ComponentSnapshotCommand<FogVolume>>(
                                    "Change FogVolume Radius", scene, ent, std::move(oldV), std::move(newV)));
                            }
                            break;
                        }
                    }

                    {
                        auto state = UI::PropertyColor("Color", v.color);
                        if (state.committed)
                            CommandHistory::Execute(std::make_unique<ComponentPropertyCommand<FogVolume, Vec3>>(
                                "Change FogVolume Color", scene, ent, &FogVolume::color,
                                UI::ConsumeItemPreEdit<Vec3>(state.itemId), v.color));
                    }

                    {
                        auto state = UI::Property("Density", v.density, 0.01f, 0.0f, 100.0f);
                        if (state.committed)
                            CommandHistory::Execute(std::make_unique<ComponentPropertyCommand<FogVolume, float>>(
                                "Change FogVolume Density", scene, ent, &FogVolume::density,
                                UI::ConsumeItemPreEdit<float>(state.itemId), v.density));
                    }
                    {
                        auto state = UI::Property("Falloff Start", v.falloffStart, 0.01f, 0.0f, 1.0f);
                        if (state.committed)
                            CommandHistory::Execute(std::make_unique<ComponentPropertyCommand<FogVolume, float>>(
                                "Change FogVolume Falloff Start", scene, ent, &FogVolume::falloffStart,
                                UI::ConsumeItemPreEdit<float>(state.itemId), v.falloffStart));
                    }
                    {
                        auto state = UI::Property("Falloff End", v.falloffEnd, 0.01f, 0.0f, 1.0f);
                        if (state.committed)
                            CommandHistory::Execute(std::make_unique<ComponentPropertyCommand<FogVolume, float>>(
                                "Change FogVolume Falloff End", scene, ent, &FogVolume::falloffEnd,
                                UI::ConsumeItemPreEdit<float>(state.itemId), v.falloffEnd));
                    }
                    {
                        auto state = UI::Property("Affects Ambient", v.affectsAmbient);
                        if (state.committed)
                            CommandHistory::Execute(std::make_unique<ComponentPropertyCommand<FogVolume, bool>>(
                                "Toggle FogVolume Ambient", scene, ent, &FogVolume::affectsAmbient,
                                UI::ConsumeItemPreEdit<bool>(state.itemId), v.affectsAmbient));
                    }

                    UI::EndProperties();
                }
            },
            std::move(opts));
    }
}
