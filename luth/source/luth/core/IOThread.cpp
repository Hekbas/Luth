#include "luthpch.h"
#include "IOThread.h"
#include "luth/core/Log.h"
#include "luth/core/JobSystem.h"
#include "luth/core/Profiler.h"

#include <fstream>

namespace Luth
{
    std::thread IOThread::s_Thread;
    std::atomic<bool> IOThread::s_Running = false;
    std::deque<IOThread::Request> IOThread::s_Queue;
    std::mutex IOThread::s_QueueLock;
    std::condition_variable IOThread::s_WakeCondition;

    void IOThread::Init()
    {
        if (s_Running) return;
        s_Running = true;
        s_Thread = std::thread(ThreadEntryPoint);
        LH_CORE_INFO("I/O Thread Initialized");
    }

    void IOThread::Shutdown()
    {
        if (!s_Running) return;
        s_Running = false;
        s_WakeCondition.notify_all();
        if (s_Thread.joinable()) s_Thread.join();
    }

    void IOThread::ReadFile(const std::string& path, std::function<void(std::vector<u8>)> callback)
    {
        {
            std::lock_guard<std::mutex> lock(s_QueueLock);
            s_Queue.push_back({ path, callback });
        }
        s_WakeCondition.notify_one();
    }

    void IOThread::ThreadEntryPoint()
    {
        LH_PROFILE_THREAD("IO Thread");

        while (s_Running)
        {
            Request req;
            bool found = false;

            {
                std::unique_lock<std::mutex> lock(s_QueueLock);
                s_WakeCondition.wait(lock, [] { return !s_Queue.empty() || !s_Running; });

                if (!s_Running && s_Queue.empty()) break;

                if (!s_Queue.empty())
                {
                    req = s_Queue.front();
                    s_Queue.pop_front();
                    found = true;
                }
            }

            if (found)
            {
                LH_PROFILE_SCOPE("IO Read");
                // Blocking Read
                std::ifstream file(req.Path, std::ios::ate | std::ios::binary);

                if (!file.is_open())
                {
                    LH_CORE_ERROR("IOThread: Failed to open file: {0}", req.Path);
                    // Callback with empty buffer
                    // Dispatch to JobSystem to avoid blocking IO thread with callback logic
                    // Note: We need to copy the callback and buffer.
                    // Since buffer is empty, it's cheap.
                    // But std::function copy might allocate.
                    
                    // We use a lambda wrapper for the job
                    // IMPORTANT: The lambda must be copyable or we need to allocate it.
                    // JobSystem::Execute takes void*.
                    
                    // For simplicity in this prototype, we just run the callback here if it's fast?
                    // NO. Callback might parse JSON or decompress textures. Must be on Worker.
                    
                    // We allocate a struct to pass to the job
                    struct JobData { std::function<void(std::vector<u8>)> cb; std::vector<u8> data; };
                    JobData* jobData = new JobData{ req.Callback, {} };
                    
                    JobSystem::Execute([](JobSystem::JobArgs args) {
                        JobData* d = (JobData*)args.data;
                        d->cb(std::move(d->data));
                        delete d;
                    }, jobData);
                    
                    continue;
                }

                size_t fileSize = (size_t)file.tellg();
                std::vector<u8> buffer(fileSize);
                file.seekg(0);
                file.read((char*)buffer.data(), fileSize);
                file.close();

                // Dispatch Callback to JobSystem (Low Priority)
                struct JobData { std::function<void(std::vector<u8>)> cb; std::vector<u8> data; };
                JobData* jobData = new JobData{ req.Callback, std::move(buffer) };

                // TODO: Use Low Priority when available in Execute API
                JobSystem::Execute([](JobSystem::JobArgs args) {
                    JobData* d = (JobData*)args.data;
                    d->cb(std::move(d->data));
                    delete d;
                }, jobData);
            }
        }
    }
}
