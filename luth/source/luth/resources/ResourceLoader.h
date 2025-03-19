#pragma once

#include "luth/renderer/Mesh.h"
#include "luth/renderer/Texture.h"

#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include <json/json.hpp>
#include <filesystem>
#include <memory>

namespace Luth
{
	namespace fs = std::filesystem;

	class ResourceLoader
	{
	public:
		template<typename T>
		static std::shared_ptr<T> Load(const fs::path& path);

	private:
		static void ProcessNode(aiNode* node, const aiScene* scene,
			const fs::path& modelPath, std::vector<std::shared_ptr<Mesh>>& meshes);
		static std::shared_ptr<Mesh> ProcessMesh(aiMesh* ai_mesh, const aiScene* scene,
			const fs::path& modelPath);
		static std::shared_ptr<Texture> LoadMaterialTexture(aiMaterial* mat, aiTextureType type,
			const fs::path& modelPath);
	};
}
