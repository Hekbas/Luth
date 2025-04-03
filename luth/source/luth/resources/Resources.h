#pragma once

#include "luth/core/UUID.h"
#include "luth/resources/MetaFile.h"
#include "luth/resources/Resource.h"
#include "luth/resources/ResourceDB.h"
#include "luth/resources/FileSystem.h"
#include "luth/resources/libraries/ModelLibrary.h"
#include "luth/resources/libraries/MaterialLibrary.h"
#include "luth/resources/libraries/ShaderLibrary.h"
#include "luth/resources/libraries/TextureCache.h"

#include <regex>

namespace Luth
{
    class Model;
    class Material;
    class Shader;
    class Texture;

    class Resources
    {
    public:
        // Primary template for resource loading
        template<typename T>
        static std::shared_ptr<T> Load(const fs::path& path) {
            return Loader<T>::Load(FileSystem::GetPath(GetType<T>(), path));
        }

        // Resource search
        template<typename T>
        static std::vector<fs::path> Find(const std::string& pattern = "*", bool recursive = false) {
            const fs::path searchDir = FileSystem::GetPath(GetType<T>(), "", false);
            return FindResources(searchDir, pattern, recursive);
        }

        // Library management
        static void Init() {
            ModelLibrary::Init();
            MaterialLibrary::Init();
            ShaderLibrary::Init();
            TextureCache::Init();
        }

        static void Shutdown() {
            ModelLibrary::Shutdown();
            MaterialLibrary::Shutdown();
            ShaderLibrary::Shutdown();
            TextureCache::Shutdown();
        }

    private:
        // Resource type mapping
        template<typename T> struct TypeMap;

        template<typename T>
        static ResourceType GetType() { return TypeMap<T>::value; }

        // Resource loader implementations
        template<typename T>
        struct Loader {
            static std::shared_ptr<T> Load(const fs::path& path) = delete;
        };

        // Resource search implementation
        static std::vector<fs::path> FindResources(const fs::path& directory,
            const std::string& pattern,
            bool recursive) {
            std::vector<fs::path> results;
            const std::regex re(pattern);

            if (recursive)
            {
                auto iterator = fs::recursive_directory_iterator(directory);
                for (const auto& entry : iterator) {
                    if (entry.is_regular_file() && std::regex_match(entry.path().filename().string(), re))
                        results.push_back(entry.path());
                }
            }
            else {
                auto iterator = fs::directory_iterator(directory);
                for (const auto& entry : iterator) {
                    if (entry.is_regular_file() && std::regex_match(entry.path().filename().string(), re))
                        results.push_back(entry.path());
                }
            }
            
            return results;
        }
    };

    // Template specializations ---------------------------------------------------
    template<>
    struct Resources::TypeMap<Model> {
        static constexpr ResourceType value = ResourceType::Model;
    };

    template<>
    struct Resources::TypeMap<Material> {
        static constexpr ResourceType value = ResourceType::Material;
    };

    template<>
    struct Resources::TypeMap<Shader> {
        static constexpr ResourceType value = ResourceType::Shader;
    };

    template<>
    struct Resources::TypeMap<Texture> {
        static constexpr ResourceType value = ResourceType::Texture;
    };

    template<>
    struct Resources::Loader<Model> {
        static std::shared_ptr<Model> Load(const fs::path& path) {
            return ModelLibrary::LoadOrGet(path);
        }
    };

    template<>
    struct Resources::Loader<Material> {
        static std::shared_ptr<Material> Load(const fs::path& path) {
            return MaterialLibrary::LoadOrGet(path);
        }
    };

    template<>
    struct Resources::Loader<Shader> {
        static std::shared_ptr<Shader> Load(const fs::path& path) {
            return ShaderLibrary::LoadOrGet(path.string());
        }
    };

    template<>
    struct Resources::Loader<Texture> {
        static std::shared_ptr<Texture> Load(const fs::path& path) {
            return TextureCache::LoadOrGet(path);
        }
    };
}
