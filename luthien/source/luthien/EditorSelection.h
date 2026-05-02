#pragma once

#include "luth/core/UUID.h"
#include "luth/scene/Entity.h"

#include <vector>

namespace Luth
{
    // Static singleton holding the editor's current selection set + version stamp.
    // Mutators publish a SelectionChangedSignal via EventBus on every state change
    // (definitions in EditorSelection.cpp); subscribers tied to selection (Inspector,
    // Hierarchy highlight) react via OnEvent rather than polling GetVersion.
    class EditorSelection
    {
    public:
        static void SelectEntity(Entity entity);    // replace with single (plain click)
        static void AddEntity(Entity entity);       // shift-click extend
        static void ToggleEntity(Entity entity);    // ctrl-click toggle
        static void RemoveEntity(Entity entity);
        static void SelectResource(UUID resource);
        static void ClearSelection();

        static bool IsSelected(Entity entity)
        {
            return std::find(s_SelectedEntities.begin(), s_SelectedEntities.end(), entity)
                   != s_SelectedEntities.end();
        }

        // Primary = last-added entity. Callers that only care about one selection use this.
        static Entity GetSelectedEntity()
        {
            return s_SelectedEntities.empty() ? Entity{} : s_SelectedEntities.back();
        }

        static const std::vector<Entity>& GetSelectedEntities() { return s_SelectedEntities; }
        static u32 GetSelectionCount() { return (u32)s_SelectedEntities.size(); }

        static UUID GetSelectedResource() { return s_SelectedResource; }
        static u32 GetVersion()           { return s_Version; }

        // Raw pick = the entity under the cursor before any drill-down rewiring.
        static void SetLastRawPick(Entity e) { s_LastRawPick = e; }
        static Entity GetLastRawPick()       { return s_LastRawPick; }

    private:
        // Defined inline so subscribers' header-only paths still resolve. Mutator
        // bodies live in the .cpp where the EditorSignal/EventBus headers can be
        // pulled without inflating every translation unit that touches selection.
        friend struct EditorSelectionInternals;

        static inline std::vector<Entity> s_SelectedEntities;
        static inline Entity s_LastRawPick;
        static inline UUID   s_SelectedResource;
        static inline u32    s_Version = 0;
    };
}
