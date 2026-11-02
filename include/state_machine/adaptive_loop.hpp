#pragma once

#include "state_machine/algebra.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>

namespace state_machine {
namespace adt {

// ============================================================================
// 1. AdaptiveScheduleTracker: State-Driven Cadence & Sleep Calculator
// ============================================================================
// Bridges the state machine's `TickPolicy` (TickIdle | TickPeriodic | TickDeadline)
// to concrete sleep durations for ROS1 CallbackQueue loops and sync-runtime
// `set_period_ns` / `wake_at_ns` host calls.
class AdaptiveScheduleTracker {
  public:
    constexpr explicit AdaptiveScheduleTracker(TickPolicy initial_policy = TickIdle{}) noexcept
        : policy_(initial_policy) {}

    constexpr explicit AdaptiveScheduleTracker(double initial_period_sec) noexcept
        : policy_(TickPeriodic{initial_period_sec}) {}

    constexpr void setPolicy(TickPolicy policy) noexcept { policy_ = policy; }
    constexpr const TickPolicy& policy() const noexcept { return policy_; }

    constexpr void markStepped(double now_sec, TickPolicy next_policy) noexcept {
        last_step_sec_ = now_sec;
        has_stepped_ = true;
        policy_ = next_policy;
    }

    constexpr void markTick(double now_sec) noexcept {
        last_step_sec_ = now_sec;
        has_stepped_ = true;
    }

    constexpr double lastStepSec() const noexcept { return last_step_sec_; }
    constexpr bool hasStepped() const noexcept { return has_stepped_; }

    inline bool shouldStepNow(double now_sec, double tolerance_sec = 1e-5) const noexcept {
        if (!has_stepped_) {
            return !std::holds_alternative<TickIdle>(policy_);
        }
        const auto delay = nextWakeDelaySec(policy_, now_sec, last_step_sec_);
        return delay.has_value() && (*delay <= tolerance_sec);
    }

    inline bool due(double now_sec, const TickPolicy& policy,
                    double tolerance_sec = 1e-5) const noexcept {
        if (!has_stepped_) {
            return !std::holds_alternative<TickIdle>(policy);
        }
        const auto delay = nextWakeDelaySec(policy, now_sec, last_step_sec_);
        return delay.has_value() && (*delay <= tolerance_sec);
    }

    inline double secondsUntilNextTick(double now_sec, const TickPolicy& policy,
                                       double idle_poll_sec = 0.25) const noexcept {
        if (!has_stepped_) {
            if (std::holds_alternative<TickIdle>(policy)) {
                return std::max(0.0, idle_poll_sec);
            }
            return 0.0;
        }
        const auto delay = nextWakeDelaySec(policy, now_sec, last_step_sec_);
        if (!delay.has_value()) {
            return std::max(0.0, idle_poll_sec);
        }
        return std::clamp(*delay, 0.0, std::max(0.001, idle_poll_sec));
    }

    // Computes the maximum time a ROS CallbackQueue::callAvailable() or poll()
    // should block before the next state-machine step is due.
    // When the state is TickIdle, returns `idle_poll_sec` (e.g., 0.25s) so the
    // thread sleeps in the kernel waiting for incoming messages with ~0% CPU.
    inline double nextWaitDurationSec(double now_sec,
                                      double idle_poll_sec = 0.25) const noexcept {
        return secondsUntilNextTick(now_sec, policy_, idle_poll_sec);
    }

    inline double nextWaitDurationSec(TickPolicy next_policy,
                                      double now_sec,
                                      double idle_poll_sec = 0.25) noexcept {
        markStepped(now_sec, next_policy);
        return secondsUntilNextTick(now_sec, policy_, idle_poll_sec);
    }

    // Translates TickPolicy to `xgc2_host_api::set_period_ns` argument:
    //   0 disables the periodic timer (when TickIdle or TickDeadline).
    inline int64_t syncRuntimePeriodNs() const noexcept {
        if (const auto* p = std::get_if<TickPeriodic>(&policy_)) {
            if (p->period_sec > 0.0 && std::isfinite(p->period_sec)) {
                return std::max<int64_t>(1, static_cast<int64_t>(std::llround(p->period_sec * 1e9)));
            }
        }
        return 0;
    }

    // Translates TickPolicy to `xgc2_host_api::wake_at_ns` argument:
    //   < 0 cancels any armed deadline.
    inline int64_t syncRuntimeDeadlineNs() const noexcept {
        if (const auto* d = std::get_if<TickDeadline>(&policy_)) {
            if (d->wake_at_sec >= 0.0 && std::isfinite(d->wake_at_sec)) {
                return static_cast<int64_t>(std::llround(d->wake_at_sec * 1e9));
            }
        }
        return -1;
    }

  private:
    TickPolicy policy_{TickIdle{}};
    double last_step_sec_{0.0};
    bool has_stepped_{false};
};

// ============================================================================
// 2. DemandWaiter: Zero-Spin Condition-Variable Scheduler for Worker/Main Loops
// ============================================================================
// Allows a loop to sleep with 0% CPU when `TickPolicy` is `TickIdle`, waking
// immediately when an external producer or async solver calls `wake()`, or when
// the active state's `TickPeriodic` / `TickDeadline` expires.
class DemandWaiter {
  public:
    DemandWaiter() = default;

    void wake() noexcept {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ++wake_revision_;
        }
        cv_.notify_one();
    }

    void requestStop() noexcept {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
            ++wake_revision_;
        }
        cv_.notify_all();
    }

    uint64_t currentRevision() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return wake_revision_;
    }

    // Waits until `wake()` is called (revision advances beyond `observed_revision`),
    // `requestStop()` is called, or the delay required by `policy` elapses.
    bool wait(uint64_t observed_revision,
              const TickPolicy& policy,
              double now_sec,
              double last_step_sec) {
        std::unique_lock<std::mutex> lock(mutex_);
        auto predicate = [this, observed_revision] {
            return stopping_ || wake_revision_ != observed_revision;
        };
        if (predicate()) {
            return !stopping_;
        }

        const auto delay_sec = nextWakeDelaySec(policy, now_sec, last_step_sec);
        if (!delay_sec.has_value()) {
            // TickIdle: sleep indefinitely until an event/wake arrives!
            cv_.wait(lock, predicate);
            return !stopping_;
        }
        if (*delay_sec <= 0.0) {
            return !stopping_;
        }
        const auto wait_ns = std::chrono::nanoseconds(
            std::max<int64_t>(1, static_cast<int64_t>(std::llround(*delay_sec * 1e9))));
        cv_.wait_for(lock, wait_ns, predicate);
        return !stopping_;
    }

  private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    uint64_t wake_revision_{0};
    bool stopping_{false};
};

} // namespace adt
} // namespace state_machine
