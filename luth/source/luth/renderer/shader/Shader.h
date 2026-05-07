#pragma once

#include "luth/core/types/LuthTypes.h"
#include "luth/resources/Asset.h"
#include "luth/renderer/resources/Buffer.h" // For ShaderDataType

#include <string>
#include <unordered_map>
#include <vector>

namespace Luth
{
	// A single pipeline stage a shader occupies. Each shader asset holds exactly one.
	enum class ShaderStage : u32
	{
		Unknown  = 0,
		Vertex   = 1,
		Fragment = 2,
		Compute  = 3,
		// Future: Geometry, TessControl, TessEval, Mesh, Task, Raygen, ...
	};

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

	// Abstract shader asset — one ShaderStage per asset, plus the introspected uniform / sampler
	// metadata that PipelineManager reads to build descriptor-set layouts. The concrete
	// VulkanShader holds the SPIR-V blob and the reflected layout. ShaderLibrary owns lifetime.
	class Shader : public Asset
	{
	public:
		virtual AssetType GetType() const override { return AssetType::Shader; }
		virtual ~Shader() = default;

		virtual ShaderStage GetStage() const = 0;
		virtual const std::vector<u32>& GetSpirV() const = 0;
		virtual const fs::path& GetPath() const = 0;
		virtual bool IsValid() const = 0;
		virtual void Reload() {}

		// Reflection Data Access
		const std::unordered_map<std::string, ShaderBuffer>& GetBuffers() const { return m_Buffers; }
		const std::unordered_map<std::string, ShaderResource>& GetResources() const { return m_Resources; }
		const std::unordered_map<std::string, ShaderBuffer>& GetPushConstants() const { return m_PushConstants; }

		static std::shared_ptr<Shader> Create(ShaderStage stage, const std::vector<u32>& spirv, const fs::path& path);

	protected:
		std::unordered_map<std::string, ShaderBuffer> m_Buffers;
		std::unordered_map<std::string, ShaderResource> m_Resources;
		std::unordered_map<std::string, ShaderBuffer> m_PushConstants;
	};
}
