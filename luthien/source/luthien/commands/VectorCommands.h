#pragma once

#include "luthien/commands/ICommand.h"
#include "luth/core/UUID.h"
#include "luth/scene/Scene.h"
#include "luth/scene/Entity.h"
#include "luth/scene/Components.h"

namespace Luth
{
    // Pointer-to-members are re-resolved on each apply so undo/redo stays safe
    // across EnTT pool relocations.
    template<typename C, typename Elem, typename T>
    class VectorElementPropertyCommand : public ICommand
    {
    public:
        using VectorPtr = std::vector<Elem> C::*;
        using FieldPtr  = T Elem::*;

        VectorElementPropertyCommand(const char* name, Scene* scene, entt::entity entity,
                                     VectorPtr vec, size_t index, FieldPtr field,
                                     T oldValue, T newValue)
            : m_Name(name), m_Scene(scene),
              m_Vector(vec), m_Index(index), m_Field(field),
              m_OldValue(std::move(oldValue)), m_NewValue(std::move(newValue))
        {
            Entity e{ entity, scene };
            m_EntityUUID = e.GetComponent<Component::ID>().Value;
        }

        void Execute() override { Apply(m_NewValue); }
        void Undo()    override { Apply(m_OldValue); }
        void Redo()    override { Apply(m_NewValue); }
        const char* GetName() const override { return m_Name; }

        bool CanMerge(const ICommand& other) const override {
            auto* o = dynamic_cast<const VectorElementPropertyCommand<C, Elem, T>*>(&other);
            return o
                && o->m_EntityUUID == m_EntityUUID
                && o->m_Vector == m_Vector
                && o->m_Index == m_Index
                && o->m_Field == m_Field;
        }
        void MergeWith(const ICommand& other) override {
            m_NewValue = static_cast<const VectorElementPropertyCommand<C, Elem, T>&>(other).m_NewValue;
        }

    private:
        void Apply(const T& value) {
            Entity e = m_Scene->FindEntityByUUID(m_EntityUUID);
            if (!e.IsValid()) return;
            auto& comp = e.GetComponent<C>();
            auto& vec  = comp.*m_Vector;
            if (m_Index >= vec.size()) return;
            vec[m_Index].*m_Field = value;
            if constexpr (requires(C c) { c.IsDirty = true; }) {
                comp.IsDirty = true;
            }
        }

        const char* m_Name;
        Scene* m_Scene;
        UUID m_EntityUUID;
        VectorPtr m_Vector;
        size_t m_Index;
        FieldPtr m_Field;
        T m_OldValue;
        T m_NewValue;
    };

    template<typename C, typename Elem>
    class VectorInsertCommand : public ICommand
    {
    public:
        using VectorPtr = std::vector<Elem> C::*;

        VectorInsertCommand(const char* name, Scene* scene, entt::entity entity,
                            VectorPtr vec, size_t index, Elem element)
            : m_Name(name), m_Scene(scene),
              m_Vector(vec), m_Index(index), m_Element(std::move(element))
        {
            Entity e{ entity, scene };
            m_EntityUUID = e.GetComponent<Component::ID>().Value;
        }

        void Execute() override {
            Entity e = m_Scene->FindEntityByUUID(m_EntityUUID);
            if (!e.IsValid()) return;
            auto& comp = e.GetComponent<C>();
            auto& vec  = comp.*m_Vector;
            size_t idx = m_Index <= vec.size() ? m_Index : vec.size();
            vec.insert(vec.begin() + idx, m_Element);
            if constexpr (requires(C c) { c.IsDirty = true; }) { comp.IsDirty = true; }
        }
        void Undo() override {
            Entity e = m_Scene->FindEntityByUUID(m_EntityUUID);
            if (!e.IsValid()) return;
            auto& comp = e.GetComponent<C>();
            auto& vec  = comp.*m_Vector;
            if (m_Index >= vec.size()) return;
            vec.erase(vec.begin() + m_Index);
            if constexpr (requires(C c) { c.IsDirty = true; }) { comp.IsDirty = true; }
        }
        void Redo() override { Execute(); }
        const char* GetName() const override { return m_Name; }

    private:
        const char* m_Name;
        Scene* m_Scene;
        UUID m_EntityUUID;
        VectorPtr m_Vector;
        size_t m_Index;
        Elem m_Element;
    };

    template<typename C, typename Elem>
    class VectorEraseCommand : public ICommand
    {
    public:
        using VectorPtr = std::vector<Elem> C::*;

        VectorEraseCommand(const char* name, Scene* scene, entt::entity entity,
                           VectorPtr vec, size_t index)
            : m_Name(name), m_Scene(scene),
              m_Vector(vec), m_Index(index)
        {
            Entity e{ entity, scene };
            m_EntityUUID = e.GetComponent<Component::ID>().Value;
            auto& comp = e.GetComponent<C>();
            auto& vec_ref = comp.*m_Vector;
            if (m_Index < vec_ref.size())
                m_Snapshot = vec_ref[m_Index];
        }

        void Execute() override {
            Entity e = m_Scene->FindEntityByUUID(m_EntityUUID);
            if (!e.IsValid()) return;
            auto& comp = e.GetComponent<C>();
            auto& vec  = comp.*m_Vector;
            if (m_Index >= vec.size()) return;
            vec.erase(vec.begin() + m_Index);
            if constexpr (requires(C c) { c.IsDirty = true; }) { comp.IsDirty = true; }
        }
        void Undo() override {
            Entity e = m_Scene->FindEntityByUUID(m_EntityUUID);
            if (!e.IsValid()) return;
            auto& comp = e.GetComponent<C>();
            auto& vec  = comp.*m_Vector;
            size_t idx = m_Index <= vec.size() ? m_Index : vec.size();
            vec.insert(vec.begin() + idx, m_Snapshot);
            if constexpr (requires(C c) { c.IsDirty = true; }) { comp.IsDirty = true; }
        }
        void Redo() override { Execute(); }
        const char* GetName() const override { return m_Name; }

    private:
        const char* m_Name;
        Scene* m_Scene;
        UUID m_EntityUUID;
        VectorPtr m_Vector;
        size_t m_Index;
        Elem m_Snapshot{};
    };
}
