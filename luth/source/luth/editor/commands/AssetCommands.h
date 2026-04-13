#pragma once

#include "luth/editor/commands/ICommand.h"
#include "luth/core/UUID.h"
#include "luth/scene/Entity.h"

#include <nlohmann/json.hpp>

namespace Luth
{
    // ── MaterialSnapshotCommand ───────────────────────────────────────────────

    class MaterialSnapshotCommand : public ICommand
    {
    public:
        MaterialSnapshotCommand(UUID materialUUID, nlohmann::json oldState, nlohmann::json newState);
        void Execute() override;
        void Undo() override;
        void Redo() override;
        const char* GetName() const override { return "Edit Material"; }

    private:
        void ApplyState(const nlohmann::json& state);
        UUID m_MaterialUUID;
        nlohmann::json m_OldState;
        nlohmann::json m_NewState;
    };

    // ── ModelInstantiateCommand ───────────────────────────────────────────────

    class ModelInstantiateCommand : public ICommand
    {
    public:
        ModelInstantiateCommand(Scene* scene, UUID modelUUID, UUID parentUUID);
        void Execute() override;
        void Undo() override;
        void Redo() override;
        const char* GetName() const override { return "Instantiate Model"; }

        Entity GetRootEntity() const;

    private:
        Scene* m_Scene;
        UUID m_ModelUUID;
        UUID m_ParentUUID;
        UUID m_RootUUID;
        nlohmann::json m_SubtreeSnapshot;
        bool m_FirstExecution = true;
    };
}
