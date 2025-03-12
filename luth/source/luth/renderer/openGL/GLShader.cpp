#include "luthpch.h"
#include "luth/renderer/openGL/GLShader.h"

#include <glm/gtc/type_ptr.hpp>
#include <fstream>
#include <sstream>

namespace Luth
{
    GLShader::GLShader(const std::string& filePath)
    {
        std::string source = Load(filePath);
        auto shaderSources = PreProcess(source);
        Compile(shaderSources);
    }

    GLShader::GLShader(const std::string& vertexSrc, const std::string& fragmentSrc)
    {
        std::unordered_map<GLenum, std::string> sources;
        sources[GL_VERTEX_SHADER] = vertexSrc;
        sources[GL_FRAGMENT_SHADER] = fragmentSrc;
        Compile(sources);
    }

    GLShader::~GLShader()
    {
        glDeleteProgram(m_RendererID);
    }

    void GLShader::Bind() const
    {
        glUseProgram(m_RendererID);
    }

    void GLShader::Unbind() const
    {
        glUseProgram(0);
    }

    void GLShader::SetBool(const std::string& name, bool value)
    {
        GLint location = GetUniformLocation(name);
        glUniform1i(location, value ? 1 : 0);
    }

    void GLShader::SetInt(const std::string& name, int value)
    {
        GLint location = GetUniformLocation(name);
        glUniform1i(location, value);
    }

    void GLShader::SetFloat(const std::string& name, float value)
    {
        GLint location = GetUniformLocation(name);
        glUniform1f(location, value);
    }

    void GLShader::SetVec2(const std::string& name, const glm::vec2& vector)
    {
        GLint location = GetUniformLocation(name);
        glUniform2fv(location, 1, glm::value_ptr(vector));
    }

    void GLShader::SetVec3(const std::string& name, const glm::vec3& vector)
    {
        GLint location = GetUniformLocation(name);
        glUniform3fv(location, 1, glm::value_ptr(vector));
    }

    void GLShader::SetVec4(const std::string& name, const glm::vec4& vector)
    {
        GLint location = GetUniformLocation(name);
        glUniform4fv(location, 1, glm::value_ptr(vector));
    }

    void GLShader::SetMat4(const std::string& name, const glm::mat4& matrix)
    {
        GLint location = GetUniformLocation(name);
        glUniformMatrix4fv(location, 1, GL_FALSE, glm::value_ptr(matrix));
    }

    std::unordered_map<GLenum, std::string> GLShader::PreProcess(const std::string& source)
    {
        std::unordered_map<GLenum, std::string> shaderSources;
        const char* typeToken = "#type";
        size_t typeTokenLength = strlen(typeToken);
        size_t pos = source.find(typeToken, 0);

        while (pos != std::string::npos)
        {
            size_t eol = source.find_first_of("\r\n", pos);
            size_t begin = pos + typeTokenLength + 1;
            std::string type = source.substr(begin, eol - begin);

            size_t nextLinePos = source.find_first_not_of("\r\n", eol);
            pos = source.find(typeToken, nextLinePos);
            shaderSources[ShaderTypeFromString(type)] = source.substr(
                nextLinePos,
                pos - (nextLinePos == std::string::npos ? source.size() - 1 : nextLinePos)
            );
        }

        return shaderSources;
    }

    void GLShader::Compile(const std::unordered_map<GLenum, std::string>& shaderSources)
    {
        GLuint program = glCreateProgram();
        std::vector<GLuint> shaderIDs;

        for (auto& [type, source] : shaderSources)
        {
            GLuint shader = glCreateShader(type);
            const GLchar* sourceCStr = source.c_str();
            glShaderSource(shader, 1, &sourceCStr, 0);
            glCompileShader(shader);

            GLint isCompiled = 0;
            glGetShaderiv(shader, GL_COMPILE_STATUS, &isCompiled);
            if (!isCompiled)
            {
                GLint maxLength = 0;
                glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &maxLength);
                std::vector<GLchar> infoLog(maxLength);
                glGetShaderInfoLog(shader, maxLength, &maxLength, &infoLog[0]);
                glDeleteShader(shader);

                LH_CORE_ERROR("Shader compilation failed:\n{0}", infoLog.data());
                return;
            }

            glAttachShader(program, shader);
            shaderIDs.push_back(shader);
        }

        glLinkProgram(program);
        GLint isLinked = 0;
        glGetProgramiv(program, GL_LINK_STATUS, &isLinked);
        if (!isLinked)
        {
            GLint maxLength = 0;
            glGetProgramiv(program, GL_INFO_LOG_LENGTH, &maxLength);
            std::vector<GLchar> infoLog(maxLength);
            glGetProgramInfoLog(program, maxLength, &maxLength, &infoLog[0]);

            for (auto id : shaderIDs)
                glDeleteShader(id);

            glDeleteProgram(program);

            LH_CORE_ERROR("Shader linking failed: {0}", infoLog.data());
            return;
        }

        for (auto id : shaderIDs)
            glDetachShader(program, id);

        m_RendererID = program;
    }

    GLenum GLShader::ShaderTypeFromString(const std::string& type)
    {
        if (type == "vertex") return GL_VERTEX_SHADER;
        if (type == "fragment") return GL_FRAGMENT_SHADER;
        if (type == "geometry") return GL_GEOMETRY_SHADER;

        LH_CORE_ASSERT(false, "Unknown shader type!");
        return 0;
    }

    GLint GLShader::GetUniformLocation(const std::string& name)
    {
        if (m_UniformLocationCache.find(name) != m_UniformLocationCache.end())
            return m_UniformLocationCache[name];

        GLint location = glGetUniformLocation(m_RendererID, name.c_str());
        if (location == -1)
            LH_CORE_WARN("Uniform '{0}' not found!", name);

        m_UniformLocationCache[name] = location;
        return location;
    }
}
