#pragma once

#include "luth/core/LuthTypes.h"

#include <glm/glm.hpp>
#include <string>
#include <unordered_map>

namespace Luth
{
	class Shader
	{
	public:
		virtual ~Shader() = default;

        virtual void Bind() const = 0;
        virtual void Unbind() const = 0;

        virtual void SetInt(const std::string& name, int value) = 0;
        virtual void SetFloat(const std::string& name, float value) = 0;
        virtual void SetVec2(const std::string& name, const glm::vec2& vector) = 0;
        virtual void SetVec3(const std::string& name, const glm::vec3& vector) = 0;
        virtual void SetVec4(const std::string& name, const glm::vec4& vector) = 0;
        virtual void SetMat4(const std::string& name, const glm::mat4& matrix) = 0;

        static std::shared_ptr<Shader> Create(const std::string& filePath);
        static std::shared_ptr<Shader> Create(const std::string& vertexSrc, const std::string& fragmentSrc);

        static std::string Load(const std::string& filePath);

    protected:
        virtual int GetUniformLocation(const std::string& name) = 0;
	};
}
