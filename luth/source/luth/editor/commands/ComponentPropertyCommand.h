#pragma once

#include "luth/editor/commands/ICommand.h"
#include "luth/core/UUID.h"
#include "luth/scene/Scene.h"
#include "luth/scene/Entity.h"
#include "luth/scene/Components.h"

namespace Luth
{
    // ── ComponentPropertyCommand<C, T> ────────────────────────────────────────
    // Generic: change one member of component C on an entity.
    // Uses pointer-to-member so the reference is re-resolved on undo/redo
    // (safe across EnTT pool relocations).

    template<typename C, typename T>
    class ComponentPropertyCommand : public ICommand
    {
    public:
        ComponentPropertyCommand(const char* name, Scene* scene, entt::entity entity,
                                 T C::*member, T oldValue, T newValue)
            : m_Name(name), m_Scene(scene),
              m_Member(member), m_OldValue(std::move(oldValue)), m_NewValue(std::move(newValue))
        {
            Entity e{ entity, scene };
            m_EntityUUID = e.GetComponent<Component::ID>().m_ID;
        }

        void Execute() override { Apply(m_NewValue); }
        void Undo()    override { Apply(m_OldValue); }
        void Redo()    override { Apply(m_NewValue); }
        const char* GetName() const override { return m_Name; }

        bool CanMerge(const ICommand& other) const override {
            auto* o = dynamic_cast<const ComponentPropertyCommand<C, T>*>(&other);
            return o && o->m_EntityUUID == m_EntityUUID && o->m_Member == m_Member;
        }
        void MergeWith(const ICommand& other) override {
            m_NewValue = static_cast<const ComponentPropertyCommand<C, T>&>(other).m_NewValue;
        }

    private:
        void Apply(const T& value) {
            Entity e = m_Scene->FindEntityByUUID(m_EntityUUID);
            if (!e.IsValid()) return;
            auto& comp = e.GetComponent<C>();
            comp.*m_Member = value;
            if constexpr (requires(C c) { c.IsDirty = true; }) {
                comp.IsDirty = true;
            }
        }

        const char* m_Name;
        Scene* m_Scene;
        UUID m_EntityUUID;
        T C::*m_Member;
        T m_OldValue;
        T m_NewValue;
    };
}
