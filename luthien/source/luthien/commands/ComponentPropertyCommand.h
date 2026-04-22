#pragma once

#include "luthien/commands/ICommand.h"
#include "luthien/CommandHistory.h"
#include "luth/core/UUID.h"
#include "luth/scene/Scene.h"
#include "luth/scene/Entity.h"
#include "luth/scene/Components.h"

#include <memory>
#include <type_traits>

// Boilerplate cutter for the most common pattern:
//   auto old = comp.Field;
//   if (UI::Property("Label", comp.Field)) {
//       EXEC_COMPONENT_PROP("Change Field", scene, ent, Comp, Field, old, comp.Field);
//   }
// Resolves the property type via decltype(OLD) so call sites stay terse.
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
                                 T C::*member, T oldValue, T newValue)
            : m_Name(name), m_Scene(scene),
              m_Member(member), m_OldValue(std::move(oldValue)), m_NewValue(std::move(newValue))
        {
            Entity e{ entity, scene };
            m_EntityUUID = e.GetComponent<Component::ID>().Value;
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
