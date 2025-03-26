#pragma once

#include "luth/editor/Editor.h"
#include "luth/scene/Entity.h"
#include "luth/scene/Scene.h"

namespace Luth
{
    class HierarchyPanel : public Panel
    {
    public:
        HierarchyPanel(Scene* context);
        void SetContext(Scene* scene);
        void OnRender() override;

        Entity GetSelectedEntity() const { return m_Selection; }
        void SetSelectedEntity(Entity entity);

    private:
        void DrawEntityNode(Entity entity);
        void DrawEntityContextMenu(Entity entity);
        void DrawEntityCreateMenu();
        void ProcessKeyboardShortcuts();
        bool EntityMatchesFilter(Entity entity);

        Scene* m_Context = nullptr;
        Entity m_Selection;
        char m_SearchFilter[256] = "";
        bool m_ShowCreateMenu = false;
        Entity m_DraggedEntity;
    };
}
