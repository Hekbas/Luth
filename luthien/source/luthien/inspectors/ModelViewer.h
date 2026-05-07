#pragma once

#include "luth/core/UUID.h"
#include "luth/resources/importers/ModelImporter.h"
#include "luthien/widgets/ThumbnailPreviewScene.h"

namespace Luth
{
    class Model;

    // Inspector pane for Model assets. Surfaces the ModelImportSettings (axis, scaling,
    // skinning, clip extraction) and renders a small orbit preview through ThumbnailPreviewScene
    // so the user can spin the model without leaving the inspector.
    class ModelViewer
    {
    public:
        void Draw(Model& model);

    private:
        UUID m_LastModelUUID;
        ModelImportSettings m_Settings;

        // Per-ModelViewer orbit state for the pinned-footer 3D preview.
        UI::ThumbnailPreviewScene::OrbitCamera m_OrbitCam;
    };
}
