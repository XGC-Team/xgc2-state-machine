#pragma once

#include "state_machine/algebra.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace state_machine {
namespace adt {

// ============================================================================
// 1. Sliding-Window Sample Timing & Rate Statistics (Zero Heap Allocation)
// ============================================================================
template <std::size_t WindowSize = 32>
class RateWindow {
  public:
    static_assert(WindowSize >= 2, "RateWindow requires at least 2 samples");

    constexpr void reset() noexcept {
        periods_.clear();
        last_stamp_sec_ = 0.0;
        has_stamp_ = false;
        last_period_sec_ = 0.0;
        time_jumped_ = false;
    }

    constexpr void observe(double stamp_sec, double max_time_jump_sec = 0.0) noexcept {
        if (!std::isfinite(stamp_sec)) {
            return;
        }
        if (has_stamp_) {
            const double dt = stamp_sec - last_stamp_sec_;
            last_period_sec_ = dt;
            if (dt < 0.0 || (max_time_jump_sec > 0.0 && dt > max_time_jump_sec)) {
                time_jumped_ = true;
            } else if (dt > 0.0) {
                periods_.push(dt);
            }
        }
        last_stamp_sec_ = stamp_sec;
        has_stamp_ = true;
    }

    constexpr bool hasSample() const noexcept { return has_stamp_; }
    constexpr std::size_t sampleCount() const noexcept { return periods_.size(); }
    constexpr double lastStampSec() const noexcept { return last_stamp_sec_; }
    constexpr double lastPeriodSec() const noexcept { return last_period_sec_; }
    constexpr bool timeJumped() const noexcept { return time_jumped_; }
    constexpr void clearTimeJump() noexcept { time_jumped_ = false; }

    constexpr double meanPeriodSec() const noexcept {
        if (periods_.empty()) {
            return 0.0;
        }
        double sum = 0.0;
        for (std::size_t i = 0; i < periods_.size(); ++i) {
            sum += periods_[i];
        }
        return sum / static_cast<double>(periods_.size());
    }

    constexpr double rateHz() const noexcept {
        const double mean_dt = meanPeriodSec();
        return (mean_dt > 1e-9) ? (1.0 / mean_dt) : 0.0;
    }

    constexpr double periodVarianceSec() const noexcept {
        if (periods_.size() < 2) {
            return 0.0;
        }
        const double mean = meanPeriodSec();
        double sq_sum = 0.0;
        for (std::size_t i = 0; i < periods_.size(); ++i) {
            const double diff = periods_[i] - mean;
            sq_sum += diff * diff;
        }
        return sq_sum / static_cast<double>(periods_.size());
    }

    inline double periodStddevSec() const noexcept {
        return std::sqrt(periodVarianceSec());
    }

  private:
    FixedRingBuffer<double, WindowSize> periods_{};
    double last_stamp_sec_{0.0};
    double last_period_sec_{0.0};
    bool has_stamp_{false};
    bool time_jumped_{false};
};

// ============================================================================
// 2. SignalSlot<T>: Continuous Data-Plane Channel (Latest Value + Rate Stats)
// ============================================================================
// Separates high-rate continuous sensor streams (IMU, Pose, Velocity, Setpoint)
// from discrete state-machine events. Updating a SignalSlot never allocates or
// enqueues an event; guards and state ticks inspect the slot directly.
template <typename T, std::size_t WindowSize = 32>
class SignalSlot {
  public:
    using value_type = T;

    constexpr SignalSlot() = default;

    constexpr void reset() noexcept {
        value_ = T{};
        seq_ = 0;
        stamp_sec_ = 0.0;
        received_at_sec_ = 0.0;
        rate_.reset();
    }

    constexpr uint64_t publish(const T& sample,
                               double stamp_sec,
                               double received_at_sec = 0.0,
                               double max_time_jump_sec = 0.0) noexcept {
        value_ = sample;
        return recordMetadata(stamp_sec, received_at_sec, max_time_jump_sec);
    }

    constexpr uint64_t publish(T&& sample,
                               double stamp_sec,
                               double received_at_sec = 0.0,
                               double max_time_jump_sec = 0.0) noexcept {
        value_ = std::move(sample);
        return recordMetadata(stamp_sec, received_at_sec, max_time_jump_sec);
    }

    constexpr bool received() const noexcept { return seq_ > 0; }
    constexpr uint64_t seq() const noexcept { return seq_; }
    constexpr double stampSec() const noexcept { return stamp_sec_; }
    constexpr double receivedAtSec() const noexcept { return received_at_sec_; }

    constexpr const T& latest() const noexcept { return value_; }
    constexpr T& latestMutable() noexcept { return value_; }
    constexpr const T* operator->() const noexcept { return &value_; }
    constexpr const T& operator*() const noexcept { return value_; }

    constexpr const RateWindow<WindowSize>& rateWindow() const noexcept { return rate_; }
    constexpr RateWindow<WindowSize>& rateWindow() noexcept { return rate_; }

    constexpr double ageSec(double now_sec) const noexcept {
        if (!received()) {
            return INFINITY;
        }
        const double ref = (received_at_sec_ > 0.0) ? received_at_sec_ : stamp_sec_;
        return now_sec - ref;
    }

    constexpr bool isFresh(double now_sec, double timeout_sec) const noexcept {
        if (!received() || !(timeout_sec > 0.0)) {
            return false;
        }
        const double age = ageSec(now_sec);
        return std::isfinite(age) && age >= -1e-3 && age <= timeout_sec;
    }

    constexpr bool isHealthy(double now_sec,
                             double timeout_sec,
                             double min_rate_hz = 0.0) const noexcept {
        if (!isFresh(now_sec, timeout_sec)) {
            return false;
        }
        if (min_rate_hz > 0.0) {
            return rate_.rateHz() >= min_rate_hz;
        }
        return true;
    }

  private:
    constexpr uint64_t recordMetadata(double stamp_sec,
                                      double received_at_sec,
                                      double max_time_jump_sec) noexcept {
        ++seq_;
        stamp_sec_ = stamp_sec;
        received_at_sec_ = (received_at_sec > 0.0) ? received_at_sec : stamp_sec;
        rate_.observe(stamp_sec_, max_time_jump_sec);
        return seq_;
    }

    T value_{};
    uint64_t seq_{0};
    double stamp_sec_{0.0};
    double received_at_sec_{0.0};
    RateWindow<WindowSize> rate_{};
};

// ============================================================================
// 3. EdgeDetector: Converts Continuous Predicates into Discrete Edge Events
// ============================================================================
enum class EdgeKind : uint8_t {
    kNone = 0,
    kRising = 1,  // false -> true
    kFalling = 2, // true -> false
};

class EdgeDetector {
  public:
    constexpr explicit EdgeDetector(bool initial = false) noexcept
        : current_(initial), initialized_(false) {}

    constexpr void reset(bool initial = false) noexcept {
        current_ = initial;
        initialized_ = false;
    }

    constexpr EdgeKind observe(bool level) noexcept {
        if (!initialized_) {
            initialized_ = true;
            current_ = level;
            return level ? EdgeKind::kRising : EdgeKind::kFalling;
        }
        if (level == current_) {
            return EdgeKind::kNone;
        }
        current_ = level;
        return level ? EdgeKind::kRising : EdgeKind::kFalling;
    }

    constexpr bool level() const noexcept { return current_; }

  private:
    bool current_{false};
    bool initialized_{false};
};

} // namespace adt
} // namespace state_machine
