#pragma once

#include "luth/renderer/shader/Shader.h"

#include <string>
#include <unordered_map>
#include <functional>
#include <memory>

namespace Luth
{
    // Global shader registry keyed by filename. Engine shaders auto-load from luth/assets/shaders
    // on first reference; project shaders register explicitly. Hooked into ShaderWatcher so file
    // edits trigger Reload, which then notifies subsystems through the registered Reload callback
    // so they can rebuild their pipelines without stalling the live frame.
    class ShaderLibrary
    {
    public:
        static void Init();
        static void Shutdown();

        static void Register(const std::string& name, std::shared_ptr<Shader> shader);
        static std::shared_ptr<Shader> Get(const std::string& name);
        static const std::unordered_map<std::string, std::shared_ptr<Shader>>& GetAll();

        // Load an engine shader by path relative to luth/assets (e.g. "shaders/pbr.vert"),
        // register it in the library keyed by filename, and return the shader.
        // Idempotent: returns the cached entry if the filename is already registered.
        // Returns nullptr on failure.
        static std::shared_ptr<Shader> LoadEngine(const std::string& engineRelPath);

        static bool Reload(const std::string& name);
        static void SetReloadCallback(std::function<void(const std::string&)> cb);

    private:
        static std::unordered_map<std::string, std::shared_ptr<Shader>> s_Shaders;
        static std::function<void(const std::string&)> s_ReloadCallback;
    };
}
