#pragma once

#include "luth/editor/Editor.h"
#include "luth/scene/Entity.h"
#include "luth/scene/Scene.h"
#include "luth/editor/panels/InspectorPanel.h"

namespace Luth
{
    class HierarchyPanel : public Panel
    {
    public:
        HierarchyPanel(Scene* context);

        void OnInit() override;
        void OnRender() override;

        void SetContext(Scene* scene);

        Entity GetSelectedEntity() const { return m_Selection; }
        Entity* GetSelectedEntity() { return &m_Selection; }
        void SetSelectedEntity(Entity entity);

    private:
        void DrawEntityNode(Entity entity);
        void DrawEntityContextMenu(Entity entity);
        void DrawEntityCreateMenu();
        void HandleDragDrop(Entity entity, const std::string& name);
        void ProcessKeyboardShortcuts();
        bool EntityMatchesFilter(Entity entity);

        void ProcessDropResource();

    private:
        Scene* m_Context = nullptr;
        Entity m_Selection;
        Entity m_DraggedEntity;
        Entity m_RenamingEntity;

        char m_SearchFilter[256] = "";
        char m_RenameBuffer[256];
        std::string m_OriginalName;

        bool m_ShowCreateMenu = false;
    };
}
