#pragma once

#include <filesystem>
#include <functional>
#include <thread>
#include <atomic>
#include <mutex>
#include <unordered_map>

namespace fs = std::filesystem;

namespace Luth
{
    // Polling watcher on a directory tree. Runs on its own dedicated OS thread with std::mutex
    // and sleeps; it is intentionally NOT paced through the fiber system because the polling
    // semantics tolerate blocking. AssetDatabase uses it to detect external edits and then
    // forward AssetChangedSignal events into the editor.
    class FileWatcher
    {
    public:
        enum class FileStatus {
            Created,
            Modified,
            Deleted
        };

        using Callback = std::function<void(const fs::path&, FileStatus)>;

        FileWatcher(float interval = 1.0f);
        ~FileWatcher();

        void AddWatch(const fs::path& path);
        void RemoveWatch(const fs::path& path);
        void SetCallback(const Callback& callback);
        void Start(bool initialScan = false);
        void Stop();

    private:
        void WatchLoop();

        std::unordered_map<fs::path, fs::file_time_type> m_Paths;
        std::vector<fs::path> m_WatchPaths;
        Callback m_Callback;
        float m_Interval;
        std::atomic<bool> m_Running;
        std::atomic<bool> m_InitialScanComplete;
        std::thread m_WatcherThread;
        std::mutex m_Mutex;
    };
}
