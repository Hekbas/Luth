#pragma once

#include <filesystem>

namespace Luth
{
    class Scene;

    class SceneSerializer
    {
    public:
        static bool Save(const Scene& scene, const std::filesystem::path& path);
        static bool Load(Scene& scene, const std::filesystem::path& path);
    };
}
