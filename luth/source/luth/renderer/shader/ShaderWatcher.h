#pragma once

#include "luth/resources/FileWatcher.h"

#include <filesystem>
#include <mutex>
#include <set>
#include <string>

namespace Luth
{
    // Monitors shader source directories on a background thread (FileWatcher)
    // and queues shader names for reload on the main thread. Poll() drains the
    // queue via ShaderLibrary::Reload, which fires the per-pipeline rebuild
    // callback registered by RenderPipeline.
    //
    // Owned by RenderPipeline. Start() is called with the engine-shaders dir
    // from RenderPipeline::Initialize; AddProjectDir/RemoveProjectDir toggle
    // the active project's dir from App's project-lifecycle hooks; Stop()
    // runs from Shutdown. Poll() is invoked once per frame in Execute's
    // prologue so reloads land before the next graph build.
    class ShaderWatcher
    {
    public:
        void Start(const std::filesystem::path& engineShadersDir);
        void Stop();

        void AddProjectDir(const std::filesystem::path& projectShadersDir);
        void RemoveProjectDir();

        void Poll();

    private:
        void Enqueue(const std::string& shaderName);

        FileWatcher           m_Watcher;
        std::filesystem::path m_ProjectDir;
        std::mutex            m_Mutex;
        std::set<std::string> m_Pending;
    };
}
