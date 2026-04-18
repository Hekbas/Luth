#pragma once

#include "luth/core/UUID.h"
#include "luth/resources/importers/ModelImporter.h"

namespace Luth
{
    class Model;

    class ModelViewer
    {
    public:
        void Draw(Model& model);

    private:
        UUID m_LastModelUUID;
        ModelImportSettings m_Settings;
    };
}
