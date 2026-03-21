#pragma once

#include "luth/editor/Editor.h"
#include "luth/editor/EditorSelection.h"
#include "luth/editor/inspectors/ModelViewer.h"
#include "luth/editor/inspectors/MaterialEditor.h"
#include "luth/editor/inspectors/TextureEditor.h"

namespace Luth
{
    class InspectorPanel : public Panel
    {
    public:
        InspectorPanel();

        void OnInit() override;
        void OnRender() override;

    private:

        void DrawEntityComponents(Entity entity);

        template<typename T, typename UIFunction>
        void DrawComponent(const std::string& name, Entity entity, UIFunction uiFunction);

        void DrawResourceProperties(UUID resource);

    private:
        ModelViewer    m_ModelViewer;
        MaterialEditor m_MaterialEditor;
        TextureEditor  m_TextureEditor;
    };
}
