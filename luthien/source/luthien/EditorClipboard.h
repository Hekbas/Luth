#pragma once

#include <entt/entt.hpp>

#include <optional>
#include <string>

namespace Luth
{
    // Single-slot editor clipboard for component copy/paste. Last copy wins.
    // Main-thread only by ImGui contract.
    class EditorClipboard
    {
    public:
        // Stores `json` keyed by `type`. Subsequent calls overwrite the slot.
        static void SetComponent(entt::id_type type, std::string json);

        // True if the slot holds a value of exactly this type.
        static bool HasComponent(entt::id_type type);

        // Returns the stored json if the slot's type matches, else nullopt.
        static std::optional<std::string> GetComponent(entt::id_type type);

        // Drops the stored value (e.g. on project unload).
        static void Clear();
    };
}
