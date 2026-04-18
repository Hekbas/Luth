#pragma once

#include "luthien/Editor.h"
#include "luthien/EditorSelection.h"
#include "luthien/inspectors/ModelViewer.h"
#include "luthien/inspectors/MaterialEditor.h"
#include "luthien/inspectors/TextureEditor.h"
#include "luthien/inspectors/ShaderEditor.h"
#include "luthien/inspectors/SceneViewer.h"
#include "luthien/inspectors/FontViewer.h"

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

        // Lock feature — keeps inspector pinned to a specific entity
        bool   m_IsLocked = false;
        Entity m_LockedEntity;

    private:
        ModelViewer    m_ModelViewer;
        MaterialEditor m_MaterialEditor;
        TextureEditor  m_TextureEditor;
        ShaderEditor   m_ShaderEditor;
        SceneViewer    m_SceneViewer;
        FontViewer     m_FontViewer;
    };
}
