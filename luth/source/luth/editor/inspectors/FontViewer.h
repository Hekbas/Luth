#pragma once

#include "luth/core/UUID.h"

namespace Luth
{
    class FontViewer
    {
    public:
        void Draw(const UUID& fontUUID, const fs::path& fontPath);
    };
}
