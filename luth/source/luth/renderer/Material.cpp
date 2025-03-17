#include "luthpch.h"
#include "luth/renderer/Material.h"
#include "luth/renderer/openGL/GLMaterial.h"

namespace Luth
{
    void Material::SetTexture(const std::string& name, const std::shared_ptr<Texture>& texture)
    {
        m_Textures[name] = texture;
    }

    std::shared_ptr<Texture> Material::GetTexture(const std::string& name) const
    {
        const auto it = m_Textures.find(name);
        return it != m_Textures.end() ? it->second : nullptr;
    }

    void Material::SetBool(const std::string& name, bool value) { m_BoolParams[name] = value; }
    void Material::SetInt(const std::string& name, int value) { m_IntParams[name] = value; }
    void Material::SetFloat(const std::string& name, float value) { m_FloatParams[name] = value; }
    void Material::SetVec3(const std::string& name, const glm::vec3& value) { m_Vec3Params[name] = value; }
    void Material::SetVec4(const std::string& name, const glm::vec4& value) { m_Vec4Params[name] = value; }
    void Material::SetMat4(const std::string& name, const glm::mat4& value) { m_Mat4Params[name] = value; }
}
