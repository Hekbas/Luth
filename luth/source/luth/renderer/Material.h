#pragma once

#include "luth/core/LuthTypes.h"
#include "luth/resources/ResourceManager.h"
#include "luth/renderer/Shader.h"
#include "luth/renderer/Texture.h"

#include <glm/glm.hpp>
#include <memory>
#include <unordered_map>

namespace Luth
{
    class Material
    {
    public:
        virtual ~Material() = default;

        virtual void Bind() = 0;

        static std::shared_ptr<Material> Create(const fs::path& path);

        void SetShader(const std::shared_ptr<Shader>& shader) { m_Shader = shader; }
        std::shared_ptr<Shader> GetShader() const { return m_Shader; }

        void SetTexture(const std::string& name, const std::shared_ptr<Texture>& texture);
        std::shared_ptr<Texture> GetTexture(const std::string& name) const;

        // Uniform setters
        void SetBool(const std::string& name, bool value);
        void SetInt(const std::string& name, int value);
        void SetFloat(const std::string& name, float value);
        void SetVec3(const std::string& name, const glm::vec3& value);
        void SetVec4(const std::string& name, const glm::vec4& value);
        void SetMat4(const std::string& name, const glm::mat4& value);

    protected:
        std::shared_ptr<Shader> m_Shader;
        std::unordered_map<std::string, std::shared_ptr<Texture>> m_Textures;

        // Uniform storage
        std::unordered_map<std::string, float> m_FloatParams;
        std::unordered_map<std::string, int> m_IntParams;
        std::unordered_map<std::string, bool> m_BoolParams;
        std::unordered_map<std::string, glm::vec3> m_Vec3Params;
        std::unordered_map<std::string, glm::vec4> m_Vec4Params;
        std::unordered_map<std::string, glm::mat4> m_Mat4Params;

        friend class ResourceManager;
    };
}
