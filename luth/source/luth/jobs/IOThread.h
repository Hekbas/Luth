#pragma once

#include "luth/core/LuthTypes.h"
#include <string>
#include <functional>
#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <atomic>

namespace Luth
{
    // ===================================================================================
    // I/O Thread (Dedicated File Reader)
    // ===================================================================================
    // Handles blocking file I/O operations on a dedicated OS thread.
    // Fibers submit requests here and receive a callback (via JobSystem) when done.
    
    class IOThread
    {
    public:
        struct Request
        {
            std::string Path;
            // Callback executed on a Fiber Worker after read completes
            // Buffer is owned by the callback (must free it)
            std::function<void(std::vector<u8>)> Callback;
        };

        static void Init();
        static void Shutdown();

        // Submit a read request. Non-blocking.
        static void ReadFile(const std::string& path, std::function<void(std::vector<u8>)> callback);

    private:
        static void ThreadEntryPoint();

        static std::thread s_Thread;
        static std::atomic<bool> s_Running;
        static std::deque<Request> s_Queue;
        static std::mutex s_QueueLock;
        static std::condition_variable s_WakeCondition;
    };
}
