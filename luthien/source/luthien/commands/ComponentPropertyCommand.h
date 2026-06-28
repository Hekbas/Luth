#pragma once

#include "luthien/commands/ICommand.h"
#include "luthien/CommandHistory.h"
#include "luthien/MultiEdit.h"
#include "luth/core/UUID.h"
#include "luth/scene/Scene.h"
#include "luth/scene/Entity.h"
#include "luth/scene/Components.h"

#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

#define EXEC_COMPONENT_PROP(NAME, SCENE, ENTITY, COMP, MEMBER, OLD, NEW)                  \
    ::Luth::CommandHistory::Execute(std::make_unique<                                     \
        ::Luth::ComponentPropertyCommand<COMP, std::decay_t<decltype(OLD)>>>(             \
        NAME, SCENE, ENTITY, &COMP::MEMBER, OLD, NEW))

namespace Luth
{
    // Pointer-to-member is re-resolved on each apply so undo/redo stays safe
    // across EnTT pool relocations.
    template<typename C, typename T>
    class ComponentPropertyCommand : public ICommand
    {
    public:
        ComponentPropertyCommand(const char* name, Scene* scene, entt::entity entity,
                                 T C::*member, T oldValue, T newValue,
                                 std::vector<UUID> extraTargets = {})
            : m_Name(name), m_Scene(scene),
              m_Member(member), m_OldValue(std::move(oldValue)), m_NewValue(std::move(newValue))
        {
            Entity e{ entity, scene };
            m_EntityUUID = e.GetComponent<Component::ID>().Value;

            // Multi-edit broadcast: when built during the Inspector's multi-select drawer loop, fan
            // this member edit out to the other selected entities. Each keeps its own pre-edit value
            // (captured here — targets aren't mutated until Execute) so undo restores them per-entity.
            if (extraTargets.empty()) extraTargets = MultiEdit::Targets();
            ForEachExtraTarget<C>(scene, extraTargets, [&](Entity t) {
                m_TargetOld.emplace_back(t.GetComponent<Component::ID>().Value, t.GetComponent<C>().*member);
            });
        }

        void Execute() override { ApplyAll(false); }
        void Undo()    override { ApplyAll(true);  }
        void Redo()    override { ApplyAll(false); }
        const char* GetName() const override { return m_Name; }

    private:
        void ApplyTo(Entity e, const T& value) {
            if (!e.IsValid() || !e.HasComponent<C>()) return;
            auto& comp = e.GetComponent<C>();
            comp.*m_Member = value;
            if constexpr (requires(C c) { c.IsDirty = true; }) comp.IsDirty = true;
            // Only physics components have on_update listeners; a no-op elsewhere, the rebuild
            // trigger for RigidBody/Collider — without it, broadcast targets would visually
            // change but never rebuild their body.
            m_Scene->Registry().patch<C>((entt::entity)e);
        }

        void ApplyAll(bool undo) {
            ApplyTo(m_Scene->FindEntityByUUID(m_EntityUUID), undo ? m_OldValue : m_NewValue);
            for (auto& [uuid, oldVal] : m_TargetOld)
                ApplyTo(m_Scene->FindEntityByUUID(uuid), undo ? oldVal : m_NewValue);
        }

        const char* m_Name;
        Scene* m_Scene;
        UUID m_EntityUUID;
        T C::*m_Member;
        T m_OldValue;
        T m_NewValue;
        std::vector<std::pair<UUID, T>> m_TargetOld;   // per-target pre-edit value for undo
    };
}
