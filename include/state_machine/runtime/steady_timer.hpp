#pragma once

#include <chrono>

namespace state_machine {
namespace runtime {

template <typename Clock = std::chrono::steady_clock, typename Duration = std::chrono::duration<double>> class Timer {
  public:
    using ClockType = Clock;
    using DurationType = Duration;
    using TimePoint = typename Clock::time_point;

    void start() {
        start_time_ = Clock::now();
        started_ = true;
    }

    Duration elapsed() const {
        if (!started_) {
            return Duration(0);
        }
        return std::chrono::duration_cast<Duration>(Clock::now() - start_time_);
    }

    template <typename ToDuration> ToDuration elapsed() const {
        return std::chrono::duration_cast<ToDuration>(elapsed());
    }

    bool started() const { return started_; }

    void reset() { start_time_ = Clock::now(); }

    void stop() { started_ = false; }

  protected:
    TimePoint start_time_;
    bool started_{false};
};

template <typename Clock = std::chrono::steady_clock, typename Duration = std::chrono::duration<double>>
class IntervalTimer : public Timer<Clock, Duration> {
  public:
    Duration interval() {
        if (!this->started()) {
            this->start();
            return Duration(0);
        }
        const auto now = Clock::now();
        const auto interval = now - this->start_time_;
        this->start_time_ = now;
        return std::chrono::duration_cast<Duration>(interval);
    }

    template <typename ToDuration> ToDuration interval() { return std::chrono::duration_cast<ToDuration>(interval()); }
};

using SteadyTimer = Timer<std::chrono::steady_clock, std::chrono::duration<double>>;
using MilliTimer = Timer<std::chrono::steady_clock, std::chrono::milliseconds>;
using IntervalTimerD = IntervalTimer<std::chrono::steady_clock, std::chrono::duration<double>>;

} // namespace runtime
} // namespace state_machine
