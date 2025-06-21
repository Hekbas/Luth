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

    void FileWatcher::SetCallback(const Callback& callback)
    {
        m_Callback = callback;
    }

    void FileWatcher::Start(bool initialScan)
    {
        if (m_Running) return;

        m_Running = true;
        m_InitialScanComplete = !initialScan; // Set based on parameter
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
        // Perform initial scan if needed
        if (!m_InitialScanComplete) {
            std::lock_guard lock(m_Mutex);
            for (const auto& watchPath : m_WatchPaths) {
                if (!fs::exists(watchPath)) continue;

                for (auto& entry : fs::recursive_directory_iterator(watchPath)) {
                    if (entry.is_regular_file()) {
                        const auto& path = entry.path();
                        m_Paths[path] = fs::last_write_time(path);
                    }
                }
            }
            m_InitialScanComplete = true;
        }

        while (m_Running) {
            std::this_thread::sleep_for(std::chrono::duration<float>(m_Interval));

            std::lock_guard lock(m_Mutex);
            for (const auto& watchPath : m_WatchPaths) {
                if (!fs::exists(watchPath)) continue;

                // Process directory recursively
                std::vector<fs::path> currentPaths;
                for (auto& entry : fs::recursive_directory_iterator(watchPath)) {
                    if (entry.is_regular_file()) {
                        currentPaths.push_back(entry.path());
                    }
                }

                // Check for deleted files
                for (auto it = m_Paths.begin(); it != m_Paths.end();) {
                    if (std::find(currentPaths.begin(), currentPaths.end(), it->first) == currentPaths.end()) {
                        if (m_Callback) m_Callback(it->first, FileStatus::Deleted);
                        it = m_Paths.erase(it);
                    }
                    else {
                        ++it;
                    }
                }

                // Check for created/modified files
                for (const auto& path : currentPaths) {
                    auto currentWriteTime = fs::last_write_time(path);

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
