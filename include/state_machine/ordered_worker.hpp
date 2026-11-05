#pragma once

#include <condition_variable>
#include <mutex>
#include <optional>
#include <queue>
#include <thread>
#include <utility>

namespace state_machine {

// An existing lifecycle owner starts/stops this worker. Producers may post
// concurrently. Execute runs only on the worker; stop must not run there.
// Preserve FIFO, discard pending work on stop, and join the claimed work.
template <class Item, class Execute> class OrderedWorker {
  public:
    explicit OrderedWorker(Execute execute) : execute_(std::move(execute)) {}
    ~OrderedWorker() { stop(); }
    OrderedWorker(const OrderedWorker&) = delete;
    OrderedWorker& operator=(const OrderedWorker&) = delete;
    OrderedWorker(OrderedWorker&&) = delete;
    OrderedWorker& operator=(OrderedWorker&&) = delete;

    void start() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (running_)
            return;
        running_ = true;
        try {
            worker_ = std::thread(&OrderedWorker::run, this);
        } catch (...) {
            running_ = false;
            throw;
        }
    }

    void stop() {
        std::queue<Item> discarded;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            running_ = false;
            pending_.swap(discarded);
        }
        ready_.notify_all();
        if (worker_.joinable())
            worker_.join();
        // Discarded payloads are destroyed after join, outside the mutex.
    }

    void post(Item item) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!running_)
                return;
            pending_.push(std::move(item));
        }
        ready_.notify_one();
    }

  private:
    void run() {
        for (;;) {
            std::optional<Item> item;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                ready_.wait(lock, [this] {
                    return !running_ || !pending_.empty();
                });
                if (!running_)
                    return;
                item.emplace(std::move(pending_.front()));
                pending_.pop();
            }
            try {
                execute_(*item);
            } catch (...) {
                // The two existing ordered output workers isolate failed items.
            }
        }
    }

    Execute execute_;
    std::mutex mutex_;
    std::condition_variable ready_;
    std::queue<Item> pending_;
    std::thread worker_;
    bool running_{false};
};

} // namespace state_machine
