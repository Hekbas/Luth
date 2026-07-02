#include "luthpch.h"
#include "luth/resources/FileWatcher.h"

#include <algorithm>

namespace Luth
{
    FileWatcher::FileWatcher(float interval) : m_Interval(interval), m_Running(false), m_InitialScanComplete(false) {}

    FileWatcher::~FileWatcher()
    {
        Stop();
    }

    void FileWatcher::AddWatch(const fs::path& path)
    {
        std::lock_guard lock(m_Mutex);
        if (std::find(m_WatchPaths.begin(), m_WatchPaths.end(), path) == m_WatchPaths.end()) {
            m_WatchPaths.push_back(path);
        }
    }

    void FileWatcher::RemoveWatch(const fs::path& path)
    {
        std::lock_guard lock(m_Mutex);
        auto it = std::find(m_WatchPaths.begin(), m_WatchPaths.end(), path);
        if (it != m_WatchPaths.end())
            m_WatchPaths.erase(it);

        // Drop tracked files that lived under this watch root so the next poll
        // doesn't report them as Deleted (they're no longer watched).
        const std::string prefix = path.lexically_normal().string();
        for (auto pit = m_Paths.begin(); pit != m_Paths.end();) {
            std::string p = pit->first.lexically_normal().string();
            if (p.rfind(prefix, 0) == 0)
                pit = m_Paths.erase(pit);
            else
                ++pit;
        }
    }

    void FileWatcher::SetCallback(const Callback& callback)
    {
        m_Callback = callback;
    }

    void FileWatcher::Start(bool initialScan)
    {
        if (m_Running) return;

        m_Running = true;
        m_InitialScanComplete = !initialScan;
        m_WatcherThread = std::thread(&FileWatcher::WatchLoop, this);
    }

    void FileWatcher::Stop()
    {
        m_Running = false;
        if (m_WatcherThread.joinable()) {
            m_WatcherThread.join();
        }
    }

    void FileWatcher::WatchLoop()
    {
        if (!m_InitialScanComplete) {
            LH_PROFILE_SCOPE("InitialScan");
            std::lock_guard lock(m_Mutex);
            for (const auto& watchPath : m_WatchPaths) {
                if (!fs::exists(watchPath)) continue;

                try {
                    for (auto& entry : fs::recursive_directory_iterator(
                        watchPath, fs::directory_options::skip_permission_denied))
                    {
                        if (entry.is_regular_file()) {
                            std::error_code ec;
                            auto wt = fs::last_write_time(entry.path(), ec);
                            if (!ec)
                                m_Paths[entry.path()] = wt;
                        }
                    }
                } catch (...) {}
            }
            m_InitialScanComplete = true;
        }

        while (m_Running) {
            std::this_thread::sleep_for(std::chrono::duration<float>(m_Interval));

            LH_PROFILE_SCOPE("PollScan");
            std::lock_guard lock(m_Mutex);
            for (const auto& watchPath : m_WatchPaths) {
                if (!fs::exists(watchPath)) continue;

                // Process directory recursively; guarded against mid-traversal deletion
                std::vector<fs::path> currentPaths;
                try {
                    for (auto& entry : fs::recursive_directory_iterator(
                        watchPath, fs::directory_options::skip_permission_denied))
                    {
                        if (entry.is_regular_file())
                            currentPaths.push_back(entry.path());
                    }
                } catch (...) {
                    // Directory deleted mid-scan; treat all tracked files as potentially stale. Next poll reconciles.
                    continue;
                }

                // Check for deleted files; only consider files under this watchPath
                std::unordered_set<fs::path> currentSet(currentPaths.begin(), currentPaths.end());
                std::string watchPrefix = watchPath.lexically_normal().string();
                for (auto it = m_Paths.begin(); it != m_Paths.end();) {
                    std::string p = it->first.lexically_normal().string();
                    bool underThisWatch = (p.rfind(watchPrefix, 0) == 0);
                    if (underThisWatch && currentSet.find(it->first) == currentSet.end()) {
                        if (m_Callback) m_Callback(it->first, FileStatus::Deleted);
                        it = m_Paths.erase(it);
                    }
                    else {
                        ++it;
                    }
                }

                // Check for created/modified files
                for (const auto& path : currentPaths) {
                    std::error_code ec;
                    auto currentWriteTime = fs::last_write_time(path, ec);
                    if (ec) continue; // File vanished between scan and timestamp read; skip

                    if (!m_Paths.count(path)) {
                        m_Paths[path] = currentWriteTime;
                        if (m_Callback) m_Callback(path, FileStatus::Created);
                    }
                    else if (m_Paths[path] != currentWriteTime) {
                        m_Paths[path] = currentWriteTime;
                        if (m_Callback) m_Callback(path, FileStatus::Modified);
                    }
                }
            }
        }
    }
}
