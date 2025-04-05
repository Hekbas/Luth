#pragma once

#include "luth/editor/Editor.h"
#include "luth/core/UUID.h"
#include "luth/scene/Entity.h"

namespace Luth
{
    class InspectorPanel : public Panel
    {
    public:
        InspectorPanel();

        void OnInit() override;
        void OnRender() override;

        void SetSelectedEntity(Entity entity) {
            m_SelectedEntity = entity;
            m_SelectedResource = {};
        }
        void SetSelectedResource(UUID resource) {
            m_SelectedResource = resource;
            m_SelectedEntity = {};
        }

    private:

        void DrawEntityComponents();
        void DrawResourceProperties();

        template<typename T, typename UIFunction>
        void DrawComponent(const std::string& name, Entity entity, UIFunction uiFunction);

    private:
        Entity m_SelectedEntity;
        UUID m_SelectedResource;
    };
}
