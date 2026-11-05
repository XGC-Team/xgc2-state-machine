#pragma once

#include <ros/callback_queue_interface.h>
#include <ros/node_handle.h>
#include <ros/timer.h>

#include <cstdint>
#include <functional>
#include <optional>
#include <utility>

namespace state_machine {
// Existing queued-owner dispatch style. The caller retains its synchronization
// boundary: request/begin/cancel are owner-local, or under its original lock.
class Ros1QueuedTurn {
    class Callback final : public ros::CallbackInterface {
      public:
        explicit Callback(std::function<void()> turn) : turn_(std::move(turn)) {}
        CallResult call() override {
            turn_();
            return Success;
        }

      private:
        std::function<void()> turn_;
    };

  public:
    Ros1QueuedTurn(ros::CallbackQueueInterface& queue, std::function<void()> turn)
        : queue_(queue), callback_(new Callback(std::move(turn))) {}
    Ros1QueuedTurn(const Ros1QueuedTurn&) = delete;
    Ros1QueuedTurn& operator=(const Ros1QueuedTurn&) = delete;
    void request() {
        if (queued_)
            return;
        queued_ = true;
        queue_.addCallback(callback_, id());
    }
    void beginTurn() { queued_ = false; }
    // Before joining producers/destroying their owner, at its original lock.
    void cancel() { queue_.removeByID(id()); }

  private:
    uint64_t id() const { return reinterpret_cast<uintptr_t>(this); }
    ros::CallbackQueueInterface& queue_;
    ros::CallbackInterfacePtr callback_;
    bool queued_{false};
};

// Only the common timer bookkeeping. The caller supplies the same converted
// duration and decides whether an already-due obligation cancels an old arm.
template <class Time> class Ros1DomainDeadline {
  public:
    Ros1DomainDeadline(ros::NodeHandle& nh, std::function<void()> notify) : notify_(std::move(notify)) {
        timer_ = nh.createTimer(ros::Duration(1), &Ros1DomainDeadline::fired, this, true, false);
    }
    Ros1DomainDeadline(const Ros1DomainDeadline&) = delete;
    Ros1DomainDeadline& operator=(const Ros1DomainDeadline&) = delete;
    template <class Delay> void arm(const Time& deadline, Delay&& delay) {
        if (armed_ && *armed_ == deadline)
            return;
        timer_.setPeriod(delay(), true);
        timer_.start();
        armed_ = deadline;
    }
    void cancel() {
        timer_.stop();
        armed_.reset();
    }

  private:
    void fired(const ros::TimerEvent&) {
        armed_.reset();
        notify_();
    }
    ros::Timer timer_;
    std::optional<Time> armed_;
    std::function<void()> notify_;
};

} // namespace state_machine
