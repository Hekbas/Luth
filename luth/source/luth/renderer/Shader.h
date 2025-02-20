# pragma once

#include <string>
#include <memory>
#include <unordered_map>

namespace Luth
{
	class Shader
	{
	public:
		virtual ~Shader() = default;

		virtual void Bind() = 0;
		virtual void UnBind();

		virtual void SetInt();
		virtual void SetFloat();
		virtual void SetVec2();
		virtual void SetVec3();
		virtual void SetVec4();

		virtual const std::string& GetName() const;

		virtual void Create(std::string path);

	private:

	};
	
	class ShaderLib
	{
	public:
		ShaderLib();
		~ShaderLib();

		void Add();
		void Remove();


	private:

		std::unordered_map<std::string, std::unique_ptr<Shader>> shaderLib;
	};
}
