#pragma once

#include "luthien/Editor.h"
#include "luth/core/UUID.h"
#include "luth/scene/Entity.h"
#include "luth/scene/Scene.h"
#include <functional>

namespace Luth
{
    class HierarchyPanel : public Panel
    {
    public:
        HierarchyPanel();

        void OnInit() override;
        void OnRender() override;

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

    private:
        std::shared_ptr<Scene> m_Context;
        Entity m_Selection;

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
