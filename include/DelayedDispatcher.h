#pragma once

#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <utility>
#include <vector>

namespace Utils
{
    class DelayedDispatcher
    {
    public:
        using Task = std::move_only_function<void()>;
        using Clock = std::chrono::steady_clock;
        using TimePoint = Clock::time_point;

        static DelayedDispatcher& Get()
        {
            static DelayedDispatcher instance;
            return instance;
        }

        template <class Rep, class Period>
        void PostDelayed(
            std::chrono::duration<Rep, Period> a_delay,
            Task&& a_task)
        {
            const auto executeAt = Clock::now() + a_delay;
            {
                std::scoped_lock lock(_mutex);
                _queue.emplace(executeAt, std::move(a_task));
            }
            _condition.notify_one();
        }

        void Stop()
        {
            _worker.request_stop();
            _condition.notify_all();
        }

    private:
        struct ScheduledTask
        {
            TimePoint time;
            mutable Task task;

            bool operator>(const ScheduledTask& a_other) const
            {
                return time > a_other.time;
            }
        };

        DelayedDispatcher()
        {
            _worker = std::jthread(
                [this](std::stop_token a_stopToken) {
                    RunLoop(std::move(a_stopToken));
                });
        }

        ~DelayedDispatcher()
        {
            Stop();
        }

        void RunLoop(std::stop_token a_stopToken)
        {
            while (!a_stopToken.stop_requested()) {
                Task task;
                {
                    std::unique_lock lock(_mutex);
                    _condition.wait(
                        lock,
                        a_stopToken,
                        [this] { return !_queue.empty(); });
                    if (a_stopToken.stop_requested()) {
                        return;
                    }

                    const auto now = Clock::now();
                    auto& next = _queue.top();
                    if (next.time <= now) {
                        task = std::move(next.task);
                        _queue.pop();
                    }
                    else {
                        const auto wakeAt = next.time;
                        _condition.wait_until(
                            lock,
                            a_stopToken,
                            wakeAt,
                            [this, wakeAt] {
                                return !_queue.empty() &&
                                    _queue.top().time < wakeAt;
                            });
                        continue;
                    }
                }

                if (task) {
                    task();
                }
            }
        }

        std::priority_queue<
            ScheduledTask,
            std::vector<ScheduledTask>,
            std::greater<>>
            _queue;
        std::mutex _mutex;
        std::condition_variable_any _condition;
        std::jthread _worker;
    };
}
