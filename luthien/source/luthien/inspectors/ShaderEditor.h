#pragma once

#include "luth/core/UUID.h"
#include <string>

namespace Luth
{
    class Shader;

    // Inline GLSL viewer and editor for a Shader asset. Caches the source text keyed by UUID so
    // panel switches don't re-read from disk on every ImGui draw call.
    class ShaderEditor
    {
    public:
        void Draw(Shader& shader);

    private:
        UUID m_LastShaderUUID;
        std::string m_SourceCode;
    };
}
