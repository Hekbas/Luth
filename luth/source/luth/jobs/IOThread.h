#pragma once

#include "luth/core/types/LuthTypes.h"
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
    // Dedicated OS thread for blocking file I/O. Fibers submit ReadFile / WriteFile requests; the
    // callback runs as a JobSystem job when the read completes, so the originating fiber doesn't
    // pay the disk latency. std::mutex + condition_variable here are deliberate; this thread
    // exists precisely so worker fibers never block on disk.

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

        struct WriteRequest
        {
            std::string Path;
            std::vector<u8> Data;
        };

        // Submit a write request. Non-blocking, fire-and-forget.
        static void WriteFile(const std::string& path, std::vector<u8> data);

        // Perf observability: running count of read callbacks dropped because all 64 callback
        // slots were busy (the JobSystem fell behind). A rising value = a stalled/never-completing load.
        static u32 GetDroppedCallbackCount();

    private:
        static void ThreadEntryPoint();
        static void FlushWrites();

        static std::thread s_Thread;
        static std::atomic<bool> s_Running;
        static std::deque<Request> s_Queue;
        static std::mutex s_QueueLock;
        static std::deque<WriteRequest> s_WriteQueue;
        static std::mutex s_WriteQueueLock;
        static std::condition_variable s_WakeCondition;
    };
}
