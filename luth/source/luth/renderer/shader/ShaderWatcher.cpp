#include "luthpch.h"
#include "luth/renderer/shader/ShaderWatcher.h"
#include "luth/renderer/shader/ShaderLibrary.h"

namespace Luth
{
    void ShaderWatcher::Start(const fs::path& engineShadersDir)
    {
        m_Watcher.AddWatch(engineShadersDir);
        m_Watcher.SetCallback([this](const fs::path& changedFile, FileWatcher::FileStatus status) {
            if (status != FileWatcher::FileStatus::Modified) return;

            std::string ext = changedFile.extension().string();
            if (ext != ".vert" && ext != ".frag" && ext != ".comp" && ext != ".slang") return;

            std::string fileName = changedFile.filename().string();
            for (const auto& [name, shader] : ShaderLibrary::GetAll())
            {
                if (shader->GetPath().filename().string() == fileName)
                {
                    Enqueue(name);
                    return;
                }
            }
            LH_LOG(Shaders, warn, "Shader watcher: changed file '{}' not registered in ShaderLibrary", fileName);
        });
        m_Watcher.Start(true);
    }

    void ShaderWatcher::Stop()
    {
        m_Watcher.Stop();
    }

    void ShaderWatcher::AddProjectDir(const fs::path& projectShadersDir)
    {
        if (!fs::exists(projectShadersDir) || !fs::is_directory(projectShadersDir))
            return;
        m_Watcher.AddWatch(projectShadersDir);
        m_ProjectDir = projectShadersDir;
        LH_LOG(Shaders, info, "Shader hot-reload watching project dir: {}", projectShadersDir.string());
    }

    void ShaderWatcher::RemoveProjectDir()
    {
        if (m_ProjectDir.empty()) return;
        m_Watcher.RemoveWatch(m_ProjectDir);
        m_ProjectDir.clear();
    }

    void ShaderWatcher::Poll()
    {
        std::lock_guard lock(m_Mutex);
        if (m_Pending.empty()) return;
        LH_PROFILE_FUNCTION();
        for (const auto& name : m_Pending)
        {
            LH_LOG(Shaders, info, "Shader file changed — reloading '{}'", name);
            ShaderLibrary::Reload(name);
            LH_PROFILE_MESSAGE(("Shader reloaded: " + name).c_str());
        }
        m_Pending.clear();
    }

    void ShaderWatcher::Enqueue(const std::string& shaderName)
    {
        std::lock_guard lock(m_Mutex);
        m_Pending.insert(shaderName);
    }
}
