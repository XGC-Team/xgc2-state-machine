#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <utility>

namespace state_machine {
namespace runtime {

template <typename ExecutionContext> class Task {
  public:
    virtual ~Task() = default;
    virtual void execute(ExecutionContext& ctx) = 0;
    virtual std::string name() const = 0;
};

template <typename ExecutionContext> class LambdaTask final : public Task<ExecutionContext> {
  public:
    using Callback = std::function<void(ExecutionContext&)>;

    LambdaTask(std::string task_name, Callback callback)
        : task_name_(std::move(task_name)), callback_(std::move(callback)) {}

    void execute(ExecutionContext& ctx) override {
        if (callback_) {
            callback_(ctx);
        }
    }

    std::string name() const override { return task_name_; }

  private:
    std::string task_name_;
    Callback callback_;
};

template <typename ExecutionContext> class AsyncTaskExecutor {
  public:
    explicit AsyncTaskExecutor(ExecutionContext& context) : context_(context) {}

    ~AsyncTaskExecutor() {
        in_destructor_.store(true);
        stop();
    }

    AsyncTaskExecutor(const AsyncTaskExecutor&) = delete;
    AsyncTaskExecutor& operator=(const AsyncTaskExecutor&) = delete;
    AsyncTaskExecutor(AsyncTaskExecutor&&) = delete;
    AsyncTaskExecutor& operator=(AsyncTaskExecutor&&) = delete;

    void start() {
        bool expected = false;
        if (!running_.compare_exchange_strong(expected, true)) {
            return;
        }
        worker_thread_ = std::thread(&AsyncTaskExecutor::workerLoop, this);
    }

    void stop() {
        std::lock_guard<std::mutex> stop_lock(stop_mutex_);
        if (!running_.exchange(false)) {
            return;
        }

        {
            std::lock_guard<std::mutex> queue_lock(queue_mutex_);
            while (!queue_.empty()) {
                queue_.pop();
            }
        }

        queue_cv_.notify_all();
        if (worker_thread_.joinable()) {
            worker_thread_.join();
        }
    }

    void pushTask(std::unique_ptr<Task<ExecutionContext>> task) {
        if (!task) {
            return;
        }

        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            if (!running_.load()) {
                return;
            }
            queue_.push(std::move(task));
        }
        queue_cv_.notify_one();
    }

    std::size_t queueSize() const {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        return queue_.size();
    }

    uint64_t executedCount() const { return tasks_executed_.load(); }
    uint64_t failedCount() const { return tasks_failed_.load(); }
    bool isRunning() const { return running_.load(); }

  private:
    void workerLoop() {
        while (true) {
            std::unique_ptr<Task<ExecutionContext>> task;
            {
                std::unique_lock<std::mutex> lock(queue_mutex_);
                queue_cv_.wait(lock, [this] {
                    return !queue_.empty() || !running_.load();
                });
                if (!running_.load() && queue_.empty()) {
                    break;
                }
                task = std::move(queue_.front());
                queue_.pop();
            }

            try {
                task->execute(context_);
                tasks_executed_.fetch_add(1);
            } catch (const std::exception&) {
                tasks_failed_.fetch_add(1);
            } catch (...) {
                tasks_failed_.fetch_add(1);
            }
        }
    }

    ExecutionContext& context_;
    std::queue<std::unique_ptr<Task<ExecutionContext>>> queue_;
    mutable std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::thread worker_thread_;
    std::atomic<bool> running_{false};
    std::mutex stop_mutex_;
    std::atomic<bool> in_destructor_{false};
    std::atomic<uint64_t> tasks_executed_{0};
    std::atomic<uint64_t> tasks_failed_{0};
};

} // namespace runtime
} // namespace state_machine
