#pragma once

#include "luth/core/LuthTypes.h"
#include "luth/core/Math.h"
#include "luth/resources/Asset.h"

#include <string>
#include <unordered_map>

namespace Luth
{
	class Shader : public Asset
	{
	public:
        virtual AssetType GetType() const override { return AssetType::Shader; }
        
		virtual ~Shader() = default;
        
        virtual const fs::path& GetPath() const = 0;

        static std::shared_ptr<Shader> Create(const fs::path& filePath);

        static std::string Load(const fs::path& filePath);
	};
}
