#include "luthpch.h"

#include "luth/physics/PhysicsMaterial.h"

#include <nlohmann/json.hpp>

namespace Luth
{
    void PhysicsMaterial::Serialize(nlohmann::json& json) const
    {
        json["friction"]    = friction;
        json["restitution"] = restitution;
        json["density"]     = density;
    }

    void PhysicsMaterial::Deserialize(const nlohmann::json& json)
    {
        friction    = json.value("friction",    0.5f);
        restitution = json.value("restitution", 0.0f);
        density     = json.value("density",     1000.0f);
    }

    const PhysicsMaterial& PhysicsMaterial::Default()
    {
        static const PhysicsMaterial sDefault;  // value-initialized — matches in-class defaults
        return sDefault;
    }
}
