#pragma once

#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <state_machine/signal_slot.hpp>
#include <thread>
#include <utility>

namespace state_machine {

// One existing compute thread. A busy or pending request is refused, never
// overwritten. Domain snapshot/admission/backend/result rules stay in callbacks.
template <class Request, class Result> class AsyncWorker {
  public:
    using Compute = std::function<Result(const Request&)>;
    using Publish = std::function<bool(const Result&)>;
    using Notify = std::function<void(const Result&)>;

    AsyncWorker(Compute compute, Publish publish, Notify notify)
        : compute_(std::move(compute)), publish_(std::move(publish)), notify_(std::move(notify)) {}
    ~AsyncWorker() { stop(); }
    AsyncWorker(const AsyncWorker&) = delete;
    AsyncWorker& operator=(const AsyncWorker&) = delete;
    AsyncWorker(AsyncWorker&&) = delete;
    AsyncWorker& operator=(AsyncWorker&&) = delete;

    void start() {
        std::optional<Request> discarded;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (worker_.joinable())
                return;
            stopped_ = false;
            busy_ = false;
            discarded = pending_.take();
            // Copy on the lifecycle caller, preserving synchronous start failures.
            Compute compute = compute_;
            worker_ = std::thread([this, compute = std::move(compute)]() mutable {
                run(compute);
            });
        }
        discarded.reset(); // Retire payloads after releasing the handoff lock.
    }

    bool trySubmit(Request request) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopped_ || busy_ || pending_.value())
                return false;
            pending_.replace(std::move(request));
        }
        ready_.notify_one();
        return true;
    }

    // Serialized lifecycle owner only. Products may retain their existing
    // recipient invalidation between close and join. Neither operation is a
    // state transition, and neither is called from this worker's callbacks.
    void close() {
        std::optional<Request> discarded;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopped_ = true;
            discarded = pending_.take();
        }
        discarded.reset();
    }
    // Precondition: close() has run; start/close/join have one serialized lifecycle owner.
    bool join() {
        ready_.notify_all();
        const bool joined = worker_.joinable();
        if (joined)
            worker_.join();
        std::lock_guard<std::mutex> lock(mutex_);
        busy_ = false;
        return joined;
    }
    bool stop() {
        close();
        return join();
    }

  private:
    void run(const Compute& compute) {
        // The callable owns this run's solver-session locals. Compute/publish
        // exceptions retain the original uncaught worker-thread behavior.
        for (;;) {
            std::optional<Request> request;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                ready_.wait(lock, [this] {
                    return stopped_ || pending_.value();
                });
                if (stopped_)
                    return;
                request = pending_.take();
                busy_ = true;
            }
            const Result result = compute(*request);
            const bool notify = publish_(result);
            {
                std::lock_guard<std::mutex> lock(mutex_);
                busy_ = false;
            }
            if (notify && notify_)
                notify_(result);
        }
    }

    Compute compute_;
    Publish publish_;
    Notify notify_;
    std::mutex mutex_;
    std::condition_variable ready_;
    std::thread worker_;
    SignalSlot<Request> pending_;
    bool stopped_{false};
    bool busy_{false};
};

} // namespace state_machine
