#include "luthpch.h"
#include "luth/renderer/openGL/GLMaterial.h"

namespace Luth
{
    void GLMaterial::Bind()
    {
        if (!m_Shader) {
            LH_CORE_WARN("Material has no shader assigned");
            return;
        }

        m_Shader->Bind();

        // Bind textures
        uint32_t textureSlot = 0;
        for (const auto& [name, texture] : m_Textures) {
            if (texture) {
                texture->Bind(textureSlot);
                m_Shader->SetInt(name, textureSlot);
                textureSlot++;
            }
        }

        // Set Uniforms
        for (const auto& [name, value] : m_BoolParams) m_Shader->SetBool(name, value);
        for (const auto& [name, value] : m_IntParams) m_Shader->SetInt(name, value);
        for (const auto& [name, value] : m_FloatParams) m_Shader->SetFloat(name, value);
        for (const auto& [name, value] : m_Vec3Params) m_Shader->SetVec3(name, value);
        for (const auto& [name, value] : m_Vec4Params) m_Shader->SetVec4(name, value);
        for (const auto& [name, value] : m_Mat4Params) m_Shader->SetMat4(name, value);
    }
}
