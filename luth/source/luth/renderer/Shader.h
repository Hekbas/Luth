#pragma once

#include "luth/core/LuthTypes.h"
#include "luth/resources/Asset.h"
#include "luth/renderer/Buffer.h" // For ShaderDataType

#include <string>
#include <unordered_map>
#include <vector>

namespace Luth
{
	// Represents a single uniform variable inside a UBO/PushConstant
	struct ShaderUniform
	{
		std::string Name;
		ShaderDataType Type = ShaderDataType::None;
		u32 Size = 0;
		u32 Offset = 0;
	};

	// Represents a Uniform Buffer (UBO) or Push Constant block
	struct ShaderBuffer
	{
		std::string Name;
		u32 Set = 0;
		u32 Binding = 0;
		u32 Size = 0;
		std::unordered_map<std::string, ShaderUniform> Uniforms;
	};

	// Represents a Texture, Sampler, or Image
	struct ShaderResource
	{
		std::string Name;
		u32 Set = 0;
		u32 Binding = 0;
		u32 ArraySize = 1;
	};

	class Shader : public Asset
	{
	public:
		virtual AssetType GetType() const override { return AssetType::Shader; }
		virtual ~Shader() = default;
        
		virtual const fs::path& GetPath() const = 0;
		virtual void Reload() {}
		virtual bool IsValid() const { return false; }

		// Reflection Data Access
		const std::unordered_map<std::string, ShaderBuffer>& GetBuffers() const { return m_Buffers; }
		const std::unordered_map<std::string, ShaderResource>& GetResources() const { return m_Resources; }
		const std::unordered_map<std::string, ShaderBuffer>& GetPushConstants() const { return m_PushConstants; }

		static std::shared_ptr<Shader> Create(const fs::path& filePath);
		static std::shared_ptr<Shader> Create(const std::vector<u32>& vertSpv, const std::vector<u32>& fragSpv, const fs::path& path);

	protected:
		std::unordered_map<std::string, ShaderBuffer> m_Buffers;
		std::unordered_map<std::string, ShaderResource> m_Resources;
		std::unordered_map<std::string, ShaderBuffer> m_PushConstants;
	};
}
