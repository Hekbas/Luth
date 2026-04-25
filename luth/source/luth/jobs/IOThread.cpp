#include "luthpch.h"
#include "luth/jobs/IOThread.h"
#include "luth/core/diagnostics/Log.h"
#include "luth/jobs/JobSystem.h"
#include "luth/core/diagnostics/Profiler.h"

#include <fstream>

namespace Luth
{
    std::thread IOThread::s_Thread;
    std::atomic<bool> IOThread::s_Running = false;
    std::deque<IOThread::Request> IOThread::s_Queue;
    std::mutex IOThread::s_QueueLock;
    std::deque<IOThread::WriteRequest> IOThread::s_WriteQueue;
    std::mutex IOThread::s_WriteQueueLock;
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

        // Flush any remaining writes that were queued after the thread stopped
        FlushWrites();
    }

    void IOThread::ReadFile(const std::string& path, std::function<void(std::vector<u8>)> callback)
    {
        {
            std::lock_guard<std::mutex> lock(s_QueueLock);
            s_Queue.push_back({ path, callback });
        }
        s_WakeCondition.notify_one();
    }

    void IOThread::WriteFile(const std::string& path, std::vector<u8> data)
    {
        {
            std::lock_guard<std::mutex> lock(s_WriteQueueLock);
            s_WriteQueue.push_back({ path, std::move(data) });
        }
        s_WakeCondition.notify_one();
    }

    void IOThread::FlushWrites()
    {
        std::deque<WriteRequest> pending;
        {
            std::lock_guard<std::mutex> lock(s_WriteQueueLock);
            pending.swap(s_WriteQueue);
        }
        for (auto& wreq : pending)
        {
            std::ofstream file(wreq.Path, std::ios::binary);
            if (file.is_open())
                file.write((const char*)wreq.Data.data(), wreq.Data.size());
            else
                LH_CORE_ERROR("IOThread: Failed to write file: {0}", wreq.Path);
        }
    }

    // -------------------------------------------------------------------------------
    // Static callback job data — avoids new/delete in the dispatch path.
    // Uses a simple ring of pre-allocated slots.
    // -------------------------------------------------------------------------------

    static constexpr u32 MAX_IO_CALLBACKS = 64;

    struct IOCallbackSlot
    {
        std::function<void(std::vector<u8>)> Callback;
        std::vector<u8> Data;
        std::atomic<bool> InUse = false;
    };

    static IOCallbackSlot s_CallbackSlots[MAX_IO_CALLBACKS];
    static std::atomic<u32> s_NextSlot = 0;

    static IOCallbackSlot* AcquireSlot()
    {
        for (u32 attempt = 0; attempt < MAX_IO_CALLBACKS; ++attempt)
        {
            u32 idx = s_NextSlot.fetch_add(1, std::memory_order_relaxed) % MAX_IO_CALLBACKS;
            bool expected = false;
            if (s_CallbackSlots[idx].InUse.compare_exchange_strong(expected, true))
                return &s_CallbackSlots[idx];
        }
        return nullptr; // All slots busy — caller should handle
    }

    static void IOCallbackJob(JobSystem::JobArgs args)
    {
        LH_PROFILE_FUNCTION();

        IOCallbackSlot* slot = (IOCallbackSlot*)args.data;
        slot->Callback(std::move(slot->Data));
        slot->Callback = nullptr;
        slot->Data.clear();
        slot->InUse.store(false, std::memory_order_release);
    }

    void IOThread::ThreadEntryPoint()
    {
        LH_PROFILE_THREAD("IO Thread");

        while (s_Running)
        {
            Request req;
            bool foundRead = false;

            {
                std::unique_lock<std::mutex> lock(s_QueueLock);
                s_WakeCondition.wait(lock, [] {
                    bool hasWrite = false;
                    { std::lock_guard<std::mutex> wl(s_WriteQueueLock); hasWrite = !s_WriteQueue.empty(); }
                    return !s_Queue.empty() || hasWrite || !s_Running;
                });

                if (!s_Running && s_Queue.empty()) { FlushWrites(); break; }

                if (!s_Queue.empty())
                {
                    req = s_Queue.front();
                    s_Queue.pop_front();
                    foundRead = true;
                }
            }

            // Process writes (fire-and-forget)
            {
                WriteRequest wreq;
                bool foundWrite = false;
                {
                    std::lock_guard<std::mutex> lock(s_WriteQueueLock);
                    if (!s_WriteQueue.empty())
                    {
                        wreq = std::move(s_WriteQueue.front());
                        s_WriteQueue.pop_front();
                        foundWrite = true;
                    }
                }
                if (foundWrite)
                {
                    LH_PROFILE_SCOPE("IO Write");
                    std::ofstream file(wreq.Path, std::ios::binary);
                    if (file.is_open())
                        file.write((const char*)wreq.Data.data(), wreq.Data.size());
                    else
                        LH_CORE_ERROR("IOThread: Failed to write file: {0}", wreq.Path);
                }
            }

            if (foundRead)
            {
                LH_PROFILE_SCOPE("IO Read");

                // Read file (blocking — that's fine, we're on a dedicated OS thread)
                std::ifstream file(req.Path, std::ios::ate | std::ios::binary);

                IOCallbackSlot* slot = AcquireSlot();
                if (!slot)
                {
                    LH_CORE_ERROR("IOThread: All callback slots busy. Dropping callback for: {0}", req.Path);
                    continue;
                }

                if (!file.is_open())
                {
                    LH_CORE_ERROR("IOThread: Failed to open file: {0}", req.Path);
                    slot->Callback = req.Callback;
                    slot->Data.clear();
                    JobSystem::Execute(IOCallbackJob, slot, nullptr, "IOCallback");
                    continue;
                }

                size_t fileSize = (size_t)file.tellg();
                slot->Data.resize(fileSize);
                file.seekg(0);
                file.read((char*)slot->Data.data(), fileSize);
                file.close();

                slot->Callback = req.Callback;
                JobSystem::Execute(IOCallbackJob, slot, nullptr, "IOCallback");
            }
        }
    }
}
