#pragma once

#include "luthien/Editor.h"
#include "luthien/EditorSelection.h"
#include "luthien/inspectors/ModelViewer.h"
#include "luthien/inspectors/MaterialEditor.h"
#include "luthien/inspectors/PhysicsMaterialEditor.h"
#include "luthien/inspectors/TextureEditor.h"
#include "luthien/inspectors/ShaderEditor.h"
#include "luthien/inspectors/SceneViewer.h"
#include "luthien/inspectors/FontViewer.h"

namespace Luth
{
    // Per-frame snapshot fragment for InspectorPanel. Captures the selection version + locked-entity
    // flag only; heavyweight component-drawer state (MaterialEditor's shader-combo enumeration) stays
    // inline in OnDraw.
    struct InspectorSnapshot
    {
        u32  selectionVersion = 0;
        bool locked = false;
    };

    class InspectorPanel : public Panel
    {
    public:
        InspectorPanel();

        void OnInit() override;
        void OnGather(EditorSnapshotBuilder& builder) override;
        void OnDraw(const EditorSnapshot& snapshot) override;

        // Component drawers call this to hand off the active material UUID
        // to the trailing MaterialEditor panel drawn after DrawEntityComponents.
        void SetActiveMaterial(UUID uuid) { m_ActiveMaterialUUID = uuid; }

    private:
        void DrawEntityComponents(Entity entity);
        void DrawResourceProperties(UUID resource);

        // When locked, the inspector stays pinned to m_LockedEntity regardless of selection.
        bool   m_IsLocked = false;
        Entity m_LockedEntity;

        // Written by the MeshRenderer drawer, read after the component loop to
        // render the trailing MaterialEditor panel. Reset at the top of each
        // DrawEntityComponents call.
        UUID m_ActiveMaterialUUID;

        ModelViewer           m_ModelViewer;
        MaterialEditor        m_MaterialEditor;
        PhysicsMaterialEditor m_PhysicsMaterialEditor;
        TextureEditor         m_TextureEditor;
        ShaderEditor          m_ShaderEditor;
        SceneViewer           m_SceneViewer;
        FontViewer            m_FontViewer;
    };
}
