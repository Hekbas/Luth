#pragma once

#include "luthien/Editor.h"
#include "luth/core/UUID.h"
#include "luth/scene/Entity.h"
#include "luth/scene/Scene.h"
#include <functional>

namespace Luth
{
    // Per-frame snapshot fragment for HierarchyPanel. Captures change-detection
    // versions only; the tree walk + filter still runs inline in OnDraw against
    // the live Scene. A later polish commit can move pre-walk + filter results
    // into this struct — the lifecycle is ready for it.
    struct HierarchySnapshot
    {
        u32 hierarchyVersion = 0;
        u32 selectionVersion = 0;
    };

    class HierarchyPanel : public Panel
    {
    public:
        HierarchyPanel();

        void OnInit() override;
        void OnGather(EditorSnapshotBuilder& builder) override;
        void OnDraw(const EditorSnapshot& snapshot) override;

        void SetContext(std::shared_ptr<Scene> scene) { m_Context = scene; }
        std::shared_ptr<Scene> GetContext() const { return m_Context; }

        Entity GetSelectedEntity() const { return m_Selection; }
        void SetSelectedEntity(Entity entity);

    private:
        void DrawTopBar();
        void DrawEntityNode(Entity entity);
        void DrawContextMenu(Entity parent = {});

        void HandleDragDropSource(Entity entity);
        void HandleDragDropTarget(Entity targetEntity);
        void HandleRootDragDropTarget();

        void RenameEntity(Entity entity);
        void DeleteSelectedEntity();
        bool IsDescendant(Entity potentialDescendant, Entity potentialAncestor);
        void InstantiateModel(UUID modelUUID, Entity parent);
        bool SubtreeMatchesFilter(Entity entity, const char* filter);

        // Shift-click range select: pick everything between the anchor and the target in the
        // flattened visible-row order (m_VisibleOrder, rebuilt each draw).
        void SelectRangeTo(Entity target);

    private:
        std::shared_ptr<Scene> m_Context;
        Entity m_Selection;

        // Anchor for shift-range select (last plain/ctrl click). m_VisibleOrder is the flattened
        // pre-order of rows actually drawn this frame (respects collapse + search filter).
        Entity m_RangeAnchor;
        std::vector<Entity> m_VisibleOrder;

        Entity m_RenamingEntity;
        bool m_IsRenaming = false;
        bool m_FocusRename = false;
        char m_RenameBuffer[256] = "";

        char m_SearchFilter[256] = "";

        std::vector<std::function<void()>> m_DeferredActions;

        struct PendingInstantiation {
            UUID ModelUUID;
            Entity Parent;
        };
        std::vector<PendingInstantiation> m_PendingInstantiations;
    };
}
