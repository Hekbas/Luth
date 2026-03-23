#pragma once

#include "luth/core/UUID.h"
#include <string>

namespace Luth
{
    class Shader;

    class ShaderEditor
    {
    public:
        void Draw(Shader& shader);

    private:
        UUID m_LastShaderUUID;
        std::string m_SourceCode;
    };
}
