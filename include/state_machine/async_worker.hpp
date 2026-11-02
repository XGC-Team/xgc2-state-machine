#pragma once

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

namespace state_machine {
namespace adt {

// ============================================================================
// AsyncLatestWorker<Request, Result>
// ============================================================================
// Single-slot, latest-wins asynchronous compute worker with state-epoch fencing.
//
// Algebraic invariant:
//   - A solver invocation belongs to a specific state residency identified by
//     `state_epoch`. If the state machine transitions before the worker finishes,
//     `state_epoch` advances and `takeResultForEpoch(current_epoch)` discards
//     the stale result automatically.
//   - When no request is pending, the worker thread blocks on a condition
//     variable with zero CPU usage.
template <typename Request, typename Result>
class AsyncLatestWorker {
  public:
    struct TaggedRequest {
        uint64_t state_epoch{0};
        uint64_t request_seq{0};
        Request payload{};
    };

    struct TaggedResult {
        uint64_t state_epoch{0};
        uint64_t request_seq{0};
        Result payload{};
    };

    using ComputeFn = std::function<Result(const Request&)>;
    using WakeFn = std::function<void()>;

    explicit AsyncLatestWorker(ComputeFn compute_fn, WakeFn wake_fn = {})
        : compute_fn_(std::move(compute_fn)), wake_fn_(std::move(wake_fn)) {}

    ~AsyncLatestWorker() noexcept { stop(); }

    AsyncLatestWorker(const AsyncLatestWorker&) = delete;
    AsyncLatestWorker& operator=(const AsyncLatestWorker&) = delete;

    void start() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (running_) {
            return;
        }
        stopping_ = false;
        running_ = true;
        worker_ = std::thread(&AsyncLatestWorker::workerLoop, this);
    }

    void stop() noexcept {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!running_) {
                return;
            }
            stopping_ = true;
            pending_.reset();
        }
        cv_.notify_all();
        if (worker_.joinable()) {
            worker_.join();
        }
        std::lock_guard<std::mutex> lock(mutex_);
        running_ = false;
        busy_ = false;
        completed_.reset();
    }

    // Submits a new request for `state_epoch`. If a previous request is still
    // waiting in the slot, it is overwritten (latest-wins coalescing).
    uint64_t submit(uint64_t state_epoch, Request request) {
        uint64_t seq = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!running_ || stopping_) {
                return 0;
            }
            seq = ++next_seq_;
            if (pending_.has_value()) {
                ++coalesced_count_;
            }
            pending_ = TaggedRequest{state_epoch, seq, std::move(request)};
        }
        cv_.notify_one();
        return seq;
    }

    // Invalidates any pending or completed work from earlier state epochs.
    void cancelEpoch() noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_.reset();
        completed_.reset();
    }

    // Takes the latest completed result if and only if its `state_epoch` matches
    // `expected_epoch`. Stale results from earlier epochs are discarded.
    std::optional<TaggedResult> takeResultForEpoch(uint64_t expected_epoch) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!completed_.has_value()) {
            return std::nullopt;
        }
        TaggedResult out = std::move(*completed_);
        completed_.reset();
        if (out.state_epoch != expected_epoch) {
            ++stale_discarded_count_;
            return std::nullopt;
        }
        return out;
    }

    bool isBusy() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return busy_ || pending_.has_value();
    }

    uint64_t coalescedCount() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return coalesced_count_;
    }

    uint64_t staleDiscardedCount() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return stale_discarded_count_;
    }

  private:
    void workerLoop() {
        for (;;) {
            TaggedRequest current;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock, [this] { return stopping_ || pending_.has_value(); });
                if (stopping_) {
                    busy_ = false;
                    return;
                }
                current = std::move(*pending_);
                pending_.reset();
                busy_ = true;
            }

            Result computed = compute_fn_(current.payload);

            bool should_wake = false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                busy_ = false;
                if (!stopping_) {
                    completed_ = TaggedResult{
                        current.state_epoch, current.request_seq, std::move(computed)};
                    should_wake = static_cast<bool>(wake_fn_);
                }
            }
            if (should_wake) {
                wake_fn_();
            }
        }
    }

    ComputeFn compute_fn_;
    WakeFn wake_fn_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::thread worker_;
    bool running_{false};
    bool stopping_{false};
    bool busy_{false};
    uint64_t next_seq_{0};
    uint64_t coalesced_count_{0};
    uint64_t stale_discarded_count_{0};
    std::optional<TaggedRequest> pending_;
    std::optional<TaggedResult> completed_;
};

} // namespace adt
} // namespace state_machine
