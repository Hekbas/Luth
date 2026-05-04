#include "lepch.h"
#include "luthien/EditorClipboard.h"

#include <utility>

namespace Luth
{
    namespace
    {
        struct Slot
        {
            entt::id_type type;
            std::string   json;
        };
        std::optional<Slot> s_Slot;
    }

    void EditorClipboard::SetComponent(entt::id_type type, std::string json)
    {
        s_Slot = Slot{ type, std::move(json) };
    }

    bool EditorClipboard::HasComponent(entt::id_type type)
    {
        return s_Slot && s_Slot->type == type;
    }

    std::optional<std::string> EditorClipboard::GetComponent(entt::id_type type)
    {
        if (!s_Slot || s_Slot->type != type) return std::nullopt;
        return s_Slot->json;
    }

    void EditorClipboard::Clear()
    {
        s_Slot.reset();
    }
}
