#include "lepch.h"
#include "luthien/EditorSelection.h"
#include "luthien/events/EditorSignals.h"
#include "luth/events/EventBus.h"
#include "luth/scene/Components.h"

#include <algorithm>

namespace Luth
{
    using namespace Component;

    // Convert the current selection set to UUIDs and enqueue a signal. Called
    // immediately after every version bump so subscribers see a coherent state.
    static void PublishSelectionChanged()
    {
        const auto& entities = EditorSelection::GetSelectedEntities();

        std::vector<UUID> uuids;
        uuids.reserve(entities.size());
        for (auto e : entities) {
            if (e.IsValid())
                uuids.push_back(e.GetComponent<ID>().Value);
        }

        EventBus::Enqueue<SelectionChangedSignal>(
            BusType::MainThread,
            EditorSelection::GetVersion(),
            std::move(uuids),
            EditorSelection::GetSelectedResource());
    }

    void EditorSelection::SelectEntity(Entity entity)
    {
        s_SelectedEntities.clear();
        if (entity)
            s_SelectedEntities.push_back(entity);
        s_SelectedResource = {};
        s_Version++;
        PublishSelectionChanged();
    }

    void EditorSelection::AddEntity(Entity entity)
    {
        if (!entity || IsSelected(entity)) return;
        s_SelectedEntities.push_back(entity);
        s_SelectedResource = {};
        s_Version++;
        PublishSelectionChanged();
    }

    void EditorSelection::ToggleEntity(Entity entity)
    {
        if (!entity) return;
        auto it = std::find(s_SelectedEntities.begin(), s_SelectedEntities.end(), entity);
        if (it != s_SelectedEntities.end())
            s_SelectedEntities.erase(it);
        else
            s_SelectedEntities.push_back(entity);
        s_SelectedResource = {};
        s_Version++;
        PublishSelectionChanged();
    }

    void EditorSelection::RemoveEntity(Entity entity)
    {
        auto it = std::find(s_SelectedEntities.begin(), s_SelectedEntities.end(), entity);
        if (it == s_SelectedEntities.end()) return;
        s_SelectedEntities.erase(it);
        s_Version++;
        PublishSelectionChanged();
    }

    void EditorSelection::SelectResource(UUID resource)
    {
        s_SelectedResource = resource;
        s_SelectedEntities.clear();
        s_Version++;
        PublishSelectionChanged();
    }

    void EditorSelection::ClearSelection()
    {
        s_SelectedEntities.clear();
        s_SelectedResource = {};
        s_LastRawPick = {};
        s_Version++;
        PublishSelectionChanged();
    }
}
