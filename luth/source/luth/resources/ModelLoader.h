#pragma once

#include "luth/renderer/Model.h"
#include "luth/renderer/SkinnedModel.h"

#include <memory>

namespace Luth
{
    class ModelLoader
    {
    public:
        static std::shared_ptr<Model> Load(const fs::path& path);

    private:
        static bool HasBones(const aiScene* scene);
    };
}
