#pragma once

#include "luth/core/types/LuthTypes.h"
#include "luth/resources/Asset.h"

#include <nlohmann/json.hpp>

namespace Luth
{
    // Per-body physical properties. Looked up by RigidBody.materialUUID at body-build time;
    // UUID::Invalid() (or a UUID whose asset hasn't loaded yet) falls back to Default().
    //
    // friction:    Coulomb friction coefficient. 0 = ice, 1 = rubber-on-asphalt range.
    // restitution: bounciness. 0 = clay, 1 = ideal elastic.
    // density:     kg/m^3. Drives mass via density * shape volume when RigidBody.mass <= 0.
    //              Routine values: 1000 = water, 7850 = steel, ~150 = soft pine. MeshShape has
    //              no well-defined volume (gated to Static where mass is irrelevant), so density
    //              there has no observable effect.
    class PhysicsMaterial : public Asset
    {
    public:
        f32 friction    = 0.5f;
        f32 restitution = 0.0f;
        f32 density     = 1000.0f;

        AssetType GetType() const override { return AssetType::PhysicsMaterial; }

        // JSON round-trip mirroring Material; survives the importer's main-thread Deserialize hop.
        void Serialize  (nlohmann::json& json) const;
        void Deserialize(const nlohmann::json& json);

        // Engine baseline for entities with materialUUID == UUID::Invalid() or for body builds before AssetManager
        // has finished loading the user-shipped Default.physmat. Static-storage so callers can hold the reference
        // across frames.
        static const PhysicsMaterial& Default();
    };
}
