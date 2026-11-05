#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>

namespace state_machine {

// One waiting owner, any existing producers. This value creates no thread.
class OwnerWake {
  public:
    using Deadline = std::optional<std::chrono::steady_clock::time_point>;
    struct Observation {
        uint64_t revision;
        bool stopped;
    };
    Observation observe() {
        std::lock_guard<std::mutex> lock(mutex_);
        return {revision_, stopped_};
    }
    void notify() { signal(false); }
    void stop() { signal(true); }
    void wait(Observation observed, Deadline deadline) {
        std::unique_lock<std::mutex> lock(mutex_);
        const auto notified = [&] {
            return stopped_ || revision_ != observed.revision;
        };
        if (deadline)
            condition_.wait_until(lock, *deadline, notified);
        else
            condition_.wait(lock, notified);
    }

  private:
    void signal(bool stopped) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ++revision_;
            stopped_ = stopped_ || stopped;
        }
        condition_.notify_one();
    }
    std::mutex mutex_;
    std::condition_variable condition_;
    uint64_t revision_{0};
    bool stopped_{false};
};

// The borrowed queue supplies dispatchOne() and empty(). Work gets a turn after
// each callback, even with more ingress queued. All business demand stays outside.
class OwnerLoop {
  public:
    explicit OwnerLoop(OwnerWake& wake) : wake_(wake) {}
    OwnerLoop(const OwnerLoop&) = delete;
    OwnerLoop& operator=(const OwnerLoop&) = delete;

    template <class Queue, class Work, class Demand, class Project, class Running>
    void run(Queue& queue, Work&& work, Demand&& demand, Project&& project, Running&& running) {
        while (running()) {
            const auto observed = wake_.observe();
            if (observed.stopped)
                break;
            queue.dispatchOne();
            work();
            const auto next = demand();
            if (!queue.empty())
                continue;
            wake_.wait(observed, project(next));
        }
    }

  private:
    OwnerWake& wake_;
};

// Preserve the existing double domain/steady demand projection. A future external
// domain obligation waits for a real clock notification, not elapsed steady time.
// Clock callables stay lazy and in the original order. Infinity means no wait limit.
template <class Demand, class DomainNow, class SteadyNow>
OwnerWake::Deadline projectOwnerDeadline(const Demand& next, bool external_clock, DomainNow&& domain_now,
                                         SteadyNow&& steady_now) {
    double wall = next.wall;
    if (next.domain <= domain_now())
        wall = std::min(wall, steady_now());
    else if (!external_clock && std::isfinite(next.domain))
        wall = std::min(wall, steady_now() + (next.domain - domain_now()));
    if (!std::isfinite(wall))
        return std::nullopt;
    const auto ns = std::chrono::nanoseconds(static_cast<int64_t>(std::ceil(static_cast<long double>(wall) * 1.0e9L)));
    return std::chrono::steady_clock::time_point(ns);
}

} // namespace state_machine
