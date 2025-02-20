#pragma once

#include "luth/core/LuthTypes.h"
#include "luth/renderer/Shader.h"

#include <unordered_map>
#include <memory>

namespace Luth
{
    class ShaderLibrary
    {
    public:
        static void Init();
        static void Shutdown();

        static void Add(const std::string& name, const std::shared_ptr<Shader>& shader);
        static std::shared_ptr<Shader> Load(const std::string& filePath);
        static std::shared_ptr<Shader> Load(const std::string& name, const std::string& filePath);
        static std::shared_ptr<Shader> Get(const std::string& name);
        static bool Exists(const std::string& name);
        static void ReloadAll();

    private:
        static inline std::unordered_map<std::string, std::shared_ptr<Shader>> s_Shaders;
    };
}
