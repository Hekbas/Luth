#include "luthpch.h"
#include "luth/scene/Entity.h"
#include "luth/scene/Scene.h"
#include "luth/scene/Components.h"

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
        if (!parent || parent == *this || IsDescendantOf(parent)) {
            LH_CORE_WARN("Invalid parenting operation");
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

        // Add to new parent
        if (parent) {
            // Get or create children component
            auto& parentChildren = parent.AddComponent<Children>().m_Children;
            parentChildren.push_back(*this);

            // Set parent component
            AddComponent<Parent>().m_Parent = parent;
        }
        else {
            RemoveComponent<Parent>();
        }

        LH_CORE_INFO("Reparented {0} to {1}", GetName(), parent.GetName());
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
}
