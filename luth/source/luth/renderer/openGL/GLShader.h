#pragma once

#include "luth/renderer/Shader.h"

#include <glad/glad.h>

namespace Luth
{
    class GLShader : public Shader
    {
    public:
        GLShader(const std::string& filePath);
        GLShader(const std::string& vertexSrc, const std::string& fragmentSrc);
        ~GLShader();

        void Bind() const override;
        void Unbind() const override;

        void SetBool(const std::string& name, bool value) override;
        void SetInt(const std::string& name, int value) override;
        void SetFloat(const std::string& name, float value) override;
        void SetVec2(const std::string& name, const glm::vec2& vector) override;
        void SetVec3(const std::string& name, const glm::vec3& vector) override;
        void SetVec4(const std::string& name, const glm::vec4& vector) override;
        void SetMat4(const std::string& name, const glm::mat4& matrix) override;

    private:
        std::unordered_map<GLenum, std::string> PreProcess(const std::string& source);
        void Compile(const std::unordered_map<GLenum, std::string>& shaderSources);

        GLenum ShaderTypeFromString(const std::string& type);
        GLint GetUniformLocation(const std::string& name);

        GLuint m_RendererID;
        std::unordered_map<std::string, GLint> m_UniformLocationCache;
    };
}
