#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace Luth
{
    class Scene;

    class SceneSerializer
    {
    public:
        // File I/O: delegates to the string variants below.
        static bool Save(const Scene& scene, const std::filesystem::path& path);
        static bool Load(Scene& scene, const std::filesystem::path& path);

        // In-memory I/O: used for play-mode scene snapshot/restore. LoadFromString with preserveAssets=true
        // keeps Scene::m_HeldAssets alive across the clear, avoiding an AssetManager reload.
        static std::string SaveToString(const Scene& scene);
        static bool LoadFromString(Scene& scene, std::string_view json, bool preserveAssets = false);
    };
}
