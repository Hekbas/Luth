#pragma once

#include "luth/core/UUID.h"

namespace Luth
{
    class Model;

    class ModelViewer
    {
    public:
        void Draw(Model& model);

    private:
        UUID m_LastModelUUID;
        float m_ScaleFactor = 1.0f;
        int m_UpAxis = 1; // 0=X, 1=Y, 2=Z
    };
}
