#pragma once

#include "luth/renderer/Material.h"

namespace Luth
{
    class GLMaterial : public Material
    {
    public:
        void Bind() override;
    };
}
