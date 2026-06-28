#pragma once

#include "luth/core/UUID.h"
#include "luth/scene/Scene.h"
#include "luth/scene/Entity.h"

#include <vector>

namespace Luth
{
    // Editor-only, main-thread broadcast context for Inspector multi-editing. While targets are
    // set, component-edit commands constructed during InspectorPanel's drawer loop (the per-member
    // ComponentPropertyCommand, plus Remove/Reset) also apply to these entities, so one edit on the
    // primary fans out to the whole selection as a single undo. Set/cleared via MultiEditScope only
    // — it must never stay set across frames, or unrelated edits would broadcast.
    class MultiEdit
    {
    public:
        static void SetTargets(std::vector<UUID> targets) { s_Targets = std::move(targets); }
        static const std::vector<UUID>& Targets() { return s_Targets; }
        static bool Active() { return !s_Targets.empty(); }
        static void Clear() { s_Targets.clear(); }

    private:
        static inline std::vector<UUID> s_Targets;
    };

    // RAII: sets broadcast targets for the enclosing scope and clears on exit, so an early return
    // or a drawer throwing can't leak the context into the next command construction.
    struct MultiEditScope
    {
        explicit MultiEditScope(std::vector<UUID> targets) { MultiEdit::SetTargets(std::move(targets)); }
        ~MultiEditScope() { MultiEdit::Clear(); }
        MultiEditScope(const MultiEditScope&) = delete;
        MultiEditScope& operator=(const MultiEditScope&) = delete;
    };

    // Resolve each broadcast target and invoke fn(Entity) for the valid ones that actually have C.
    // Used by multi-edit-capable commands both to capture per-target pre-edit values at
    // construction and to re-apply on execute/undo. Heterogeneous selections are safe — targets
    // lacking C are skipped.
    template<typename C, typename Fn>
    inline void ForEachExtraTarget(Scene* scene, const std::vector<UUID>& targets, Fn&& fn)
    {
        if (!scene) return;
        for (const UUID& u : targets) {
            Entity e = scene->FindEntityByUUID(u);
            if (e.IsValid() && e.HasComponent<C>())
                fn(e);
        }
    }
}
