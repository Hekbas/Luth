#pragma once

#include "luth/core/UUID.h"
#include "luth/scene/Entity.h"

#include <vector>
#include <algorithm>

namespace Luth
{
    class EditorSelection
    {
    public:
        // Replace selection with a single entity (plain click).
        static void SelectEntity(Entity entity) {
            s_SelectedEntities.clear();
            if (entity)
                s_SelectedEntities.push_back(entity);
            s_SelectedResource = {};
            s_Version++;
        }

        // Extend selection (Shift+Click).
        static void AddEntity(Entity entity) {
            if (!entity || IsSelected(entity)) return;
            s_SelectedEntities.push_back(entity);
            s_SelectedResource = {};
            s_Version++;
        }

        // Toggle membership (Ctrl+Click).
        static void ToggleEntity(Entity entity) {
            if (!entity) return;
            auto it = std::find(s_SelectedEntities.begin(), s_SelectedEntities.end(), entity);
            if (it != s_SelectedEntities.end())
                s_SelectedEntities.erase(it);
            else
                s_SelectedEntities.push_back(entity);
            s_SelectedResource = {};
            s_Version++;
        }

        static void RemoveEntity(Entity entity) {
            auto it = std::find(s_SelectedEntities.begin(), s_SelectedEntities.end(), entity);
            if (it != s_SelectedEntities.end()) {
                s_SelectedEntities.erase(it);
                s_Version++;
            }
        }

        static bool IsSelected(Entity entity) {
            return std::find(s_SelectedEntities.begin(), s_SelectedEntities.end(), entity) != s_SelectedEntities.end();
        }

        static void SelectResource(UUID resource) {
            s_SelectedResource = resource;
            s_SelectedEntities.clear();
            s_Version++;
        }

        static void ClearSelection() {
            s_SelectedEntities.clear();
            s_SelectedResource = {};
            s_LastRawPick = {};
            s_Version++;
        }

        // Primary = last-added entity. Callers that only care about one selection use this.
        static Entity GetSelectedEntity() {
            return s_SelectedEntities.empty() ? Entity{} : s_SelectedEntities.back();
        }

        static const std::vector<Entity>& GetSelectedEntities() { return s_SelectedEntities; }
        static u32 GetSelectionCount() { return (u32)s_SelectedEntities.size(); }

        static UUID GetSelectedResource()   { return s_SelectedResource; }
        static u32 GetVersion()             { return s_Version; }

        // Raw pick = the entity under the cursor before any drill-down rewiring.
        static void SetLastRawPick(Entity e) { s_LastRawPick = e; }
        static Entity GetLastRawPick()       { return s_LastRawPick; }

    private:
        static inline std::vector<Entity> s_SelectedEntities;
        static inline Entity s_LastRawPick;
        static inline UUID   s_SelectedResource;
        static inline u32    s_Version = 0;
    };
}
