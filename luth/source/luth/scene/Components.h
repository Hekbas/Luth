#pragma once

#include "luth/scene/Entity.h"
#include "luth/core/UUID.h"

#include <entt/entt.hpp>
#include <string>
#include <vector>

namespace Luth::Component
{
    struct ID {
        UUID m_ID;

        ID() = default;
        ID(const ID&) = default;
    };

    struct Tag {
        std::string m_Tag;

        Tag() = default;
        Tag(const Tag&) = default;
        Tag(const std::string& tag) : m_Tag(tag) {}
    };

    struct Parent {
        Entity m_Parent;
    };

    struct Children {
        std::vector<Entity> m_Children;
    };
}

namespace Luth { using namespace Luth::Component; }
