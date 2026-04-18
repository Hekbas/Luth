#pragma once

#include "luth/core/UUID.h"

namespace Luth
{
    class SceneViewer
    {
    public:
        void Draw(const UUID& sceneUUID, const fs::path& scenePath);
    };
}
