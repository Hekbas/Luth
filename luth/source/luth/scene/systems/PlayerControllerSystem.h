#pragma once

#include "luth/scene/systems/ISystem.h"

namespace Luth
{
    // stub: drives CharacterController.desiredVelocity from raw WASD/Space input until a scripting
    // layer lands. Walks every entity with a CharacterController — fine for Tier 1 single-player
    // scenes; multi-character filtering is a marker-component away when needed.
    //
    // Runs on the game-stage fiber from App::GameStageFn AFTER TransformSystem (so WorldTransform's
    // forward/right basis is fresh) and BEFORE PhysicsSystem (so desiredVelocity is set when
    // UpdateCharacters reads it). Gated by App::m_RunGameSystems so Editing mode stays inert.
    class PlayerControllerSystem final : public ISystem
    {
    public:
        void Update(Scene* scene) override;
    };
}
