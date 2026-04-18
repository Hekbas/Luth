#pragma once

#include "luth/renderer/shader/Shader.h"

#include <string>
#include <unordered_map>
#include <functional>
#include <memory>

namespace Luth
{
    class ShaderLibrary
    {
    public:
        static void Init();
        static void Shutdown();

        static void Register(const std::string& name, std::shared_ptr<Shader> shader);
        static std::shared_ptr<Shader> Get(const std::string& name);
        static const std::unordered_map<std::string, std::shared_ptr<Shader>>& GetAll();

        static bool Reload(const std::string& name);
        static void SetReloadCallback(std::function<void(const std::string&)> cb);

    private:
        static std::unordered_map<std::string, std::shared_ptr<Shader>> s_Shaders;
        static std::function<void(const std::string&)> s_ReloadCallback;
    };
}
