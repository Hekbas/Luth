#pragma once

#include <vector>
#include <thread>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <future>
#include <memory>

namespace Luth
{
    class ThreadPool
    {
    public:
        explicit ThreadPool(size_t threads = std::thread::hardware_concurrency())
        {
            for (size_t i = 0; i < threads; ++i)
                workers.emplace_back([this] { WorkLoop(); });
        }

        ~ThreadPool()
        {
            {
                std::unique_lock lock(queueMutex);
                shouldTerminate = true;
            }
            condition.notify_all();
            for (std::thread& worker : workers)
                worker.join();
        }

        template<class F>
        auto Submit(F&& task) -> std::future<decltype(task())>
        {
            using return_type = decltype(task());

            auto packaged_task = std::make_shared<std::packaged_task<return_type()>>(
                std::forward<F>(task)
            );

            std::future<return_type> future = packaged_task->get_future();

            {
                std::unique_lock lock(queueMutex);
                tasks.emplace([packaged_task]() { (*packaged_task)(); });
                pendingTasks++;
            }

            condition.notify_one();
            return future;
        }

        void WaitForAll()
        {
            std::unique_lock<std::mutex> lock(queueMutex);
            condition.wait(lock, [this] { return pendingTasks == 0; });
        }

    private:
        void WorkLoop()
        {
            while (true)
            {
                std::function<void()> task;
                {
                    std::unique_lock lock(queueMutex);
                    condition.wait(lock, [this] {
                        return !tasks.empty() || shouldTerminate; });

                    if (shouldTerminate && tasks.empty())
                        return;

                    task = std::move(tasks.front());
                    tasks.pop();
                }

                task();

                {
                    std::unique_lock lock(queueMutex);
                    pendingTasks--;
                    if (pendingTasks == 0)
                        condition.notify_all();
                }
            }
        }

        std::vector<std::thread> workers;
        std::queue<std::function<void()>> tasks;
        std::mutex queueMutex;
        std::condition_variable condition;
        bool shouldTerminate = false;
        int pendingTasks = 0;
    };
}
