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
    // Per-frame snapshot fragment for InspectorPanel. v2.9.0 captures only the
    // selection version + the locked-entity flag — sufficient for future
    // skip-gather optimization. Component-drawer state (which is heavyweight,
    // including the MaterialEditor's shader-combo enumeration noted in the
    // pre-rework Tracy capture) stays inline in OnDraw for now; a future polish
    // commit can move asset-DB lookups into gather.
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
        bool UsesNewLifecycle() const override { return true; }
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

        ModelViewer    m_ModelViewer;
        MaterialEditor m_MaterialEditor;
        TextureEditor  m_TextureEditor;
        ShaderEditor   m_ShaderEditor;
        SceneViewer    m_SceneViewer;
        FontViewer     m_FontViewer;
    };
}
