#include "luthpch.h"
#include "luth/ECS/Entity.h"
#include "luth/ECS/Components.h"

namespace Luth
{
    Entity::Entity(entt::entity handle, Scene* scene)
        : m_EntityHandle(handle), m_Scene(scene) {
    }

    std::string Entity::GetName() const
    {
        if (HasComponent<Tag>())
            return GetComponent<Tag>().m_Tag;
        return "Unnamed Entity";
    }

    void Entity::SetName(const std::string& name)
    {
        GetComponent<Tag>().m_Tag = name;
    }

    void Entity::SetParent(Entity parent)
    {
        // Prevent invalid parenting
        if (parent == *this || IsAncestorOf(parent)) {
            LH_CORE_WARN("Invalid parenting operation: Cannot parent to self or descendant");
            return;
        }

        // Remove from old parent
        if (HasComponent<Parent>()) {
            Entity oldParent = GetComponent<Parent>().m_Parent;
            if (oldParent && oldParent.HasComponent<Children>()) {
                auto& children = oldParent.GetComponent<Children>().m_Children;
                children.erase(std::remove(children.begin(), children.end(), *this), children.end());
            }
        }
        else
        {
            m_Scene->RemoveFromRoots(*this);
        }

        if (parent)
        {
            // Add to new parent
            auto& childrenComp = parent.HasComponent<Children>() ?
                parent.GetComponent<Children>() :
                parent.AddComponent<Children>();
            childrenComp.m_Children.push_back(*this);

            // Set new parent component
            AddOrReplaceComponent<Parent>().m_Parent = parent;
        }
        else
        {
            // Make root
            m_Scene->AddToRoots(*this);
            if (HasComponent<Parent>()) RemoveComponent<Parent>();
        }
    }

    Entity Entity::GetParent() const
    {
        if (HasComponent<Parent>())
            return GetComponent<Parent>().m_Parent;
        return {};
    }

    std::vector<Entity> Entity::GetChildren() const
    {
        if (HasComponent<Children>())
            return GetComponent<Children>().m_Children;
        return {};
    }

    bool Entity::IsDescendantOf(Entity potentialAncestor) const
    {
        Entity current = *this;
        while (current.HasParent()) {
            current = current.GetParent();
            if (current == potentialAncestor) return true;
        }
        return false;
    }

    bool Entity::IsAncestorOf(Entity potentialDescendant) const
    {
        if (!IsValid() || !potentialDescendant.IsValid()) return false;

        Entity current = potentialDescendant;
        while (current.HasParent()) {
            current = current.GetParent();
            if (current == *this) return true;
        }

        return false;
    }
}
