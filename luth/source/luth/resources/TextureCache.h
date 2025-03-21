#pragma once

#include "luth/renderer/Texture.h"
#include "luth/renderer/openGL/GLTexture.h"

#include <memory>

namespace Luth
{
	class TextureCache
	{
	public:
        inline static std::shared_ptr<Texture> GetTexture(const fs::path& path)
        {
            static std::unordered_map<fs::path, std::weak_ptr<Texture>> cache;

            auto& weak = cache[path];
            if (auto tex = weak.lock()) return tex;

            auto newTex = Texture::Create(path);
            weak = newTex;
            return newTex;
        }
	};
}
