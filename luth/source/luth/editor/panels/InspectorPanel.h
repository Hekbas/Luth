#pragma once

#include "luth/editor/Editor.h"
#include "luth/scene/Entity.h"

namespace Luth
{
    class InspectorPanel : public Panel
    {
    public:
        void OnInit() override;
        void OnRender() override;

        void SetSelectedEntity(Entity entity) { m_SelectedEntity = entity; }

    private:
        template<typename T, typename UIFunction>
        void DrawComponent(const std::string& name, Entity entity, UIFunction uiFunction);

    private:
        Entity m_SelectedEntity;
    };
}
