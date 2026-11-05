#pragma once

#include <ros/callback_queue.h>
#include <ros/poll_manager.h>
#include <ros/ros.h>
#include <rosgraph_msgs/Clock.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <utility>

namespace state_machine {
// One existing node owner, one callback at a time. Only actual ROS ingress, an NMPC completion,
// an existing deadline or ROS shutdown wakes it. No worker, periodic pump or simulation Timer.
class Ros1AdaptiveLoop {
    struct Wake {
        std::mutex mutex;
        std::condition_variable condition;
        uint64_t revision{0};
        bool stopped{false};
        void notify(bool shutdown = false) {
            {
                std::lock_guard<std::mutex> lock(mutex);
                ++revision;
                stopped = stopped || shutdown;
            }
            condition.notify_one();
        }
    };
    class Queue : public ros::CallbackQueue {
      public:
        explicit Queue(std::shared_ptr<Wake> wake) : wake_(std::move(wake)) {}
        void addCallback(const ros::CallbackInterfacePtr& callback, uint64_t id = 0) override {
            ros::CallbackQueue::addCallback(callback, id);
            wake_->notify();
        }

      private:
        std::shared_ptr<Wake> wake_;
    };
    struct ShutdownConnection {
        std::shared_ptr<Wake> wake;
        ~ShutdownConnection() { wake->notify(true); }
    };

  public:
    Ros1AdaptiveLoop() : wake_(std::make_shared<Wake>()), queue_(wake_) {}
    Ros1AdaptiveLoop(const Ros1AdaptiveLoop&) = delete;
    Ros1AdaptiveLoop& operator=(const Ros1AdaptiveLoop&) = delete;
    ~Ros1AdaptiveLoop() {
        clock_.shutdown();
        shutdown_.disconnect();
        queue_.disable();
        queue_.clear();
    }

    // Attach before constructing subscriptions. The caller keeps this loop alive until all its
    // producers have stopped; the shared notification value also covers a concurrent ROS shutdown.
    void attach(ros::NodeHandle& nh, bool domain_clock = true) {
        nh.setCallbackQueue(&queue_);
        simulated_ = domain_clock && ros::Time::isSimTime();
        domain_now_ = ros::Time::now().toSec();
        if (simulated_) {
            // Same latest-value clock subscription as roscpp. Use the delivered value on this
            // owner so a last /clock followed by pause cannot race its internal queue's update.
            clock_ =
                nh.subscribe<rosgraph_msgs::Clock>("/clock", 1, [this](const rosgraph_msgs::Clock::ConstPtr& sample) {
                    domain_now_ = sample->clock.toSec();
                });
        }
        // Noetic PollManager::shutdown disconnects all slots, including later listeners before
        // they can inspect ros::ok(). Observe the captured connection lifetime instead. The
        // empty slot does no polling work and creates no additional thread or owner wake.
        auto token = std::make_shared<ShutdownConnection>();
        token->wake = wake_;
        shutdown_ = ros::PollManager::instance()->addPollThreadListener([token] {});
    }
    ros::CallbackQueueInterface* queue() { return &queue_; }
    bool externalClock() const { return simulated_; }
    double now() const { return simulated_ ? domain_now_ : ros::Time::now().toSec(); }
    static double wallNow() {
        return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }
    void notify() { wake_->notify(); }

    template <class Work, class Demand> void run(Work&& work, Demand&& demand) {
        while (ros::ok() && !ros::isShuttingDown()) {
            uint64_t observed;
            {
                std::lock_guard<std::mutex> lock(wake_->mutex);
                if (wake_->stopped)
                    break;
                observed = wake_->revision;
            }
            queue_.callOne(ros::WallDuration(0));
            work();
            const auto next = demand();
            if (!queue_.isEmpty())
                continue;
            double wall = next.wall;
            if (next.domain <= now())
                wall = std::min(wall, wallNow());
            else if (!simulated_ && std::isfinite(next.domain))
                wall = std::min(wall, wallNow() + (next.domain - now()));
            std::unique_lock<std::mutex> lock(wake_->mutex);
            const auto notified = [&] {
                return wake_->stopped || wake_->revision != observed;
            };
            if (std::isfinite(wall)) {
                const auto ns =
                    std::chrono::nanoseconds(static_cast<int64_t>(std::ceil(static_cast<long double>(wall) * 1.0e9L)));
                wake_->condition.wait_until(lock, std::chrono::steady_clock::time_point(ns), notified);
            } else {
                wake_->condition.wait(lock, notified);
            }
        }
    }

  private:
    std::shared_ptr<Wake> wake_;
    Queue queue_;
    boost::signals2::connection shutdown_;
    ros::Subscriber clock_;
    bool simulated_{false};
    double domain_now_{0};
};
} // namespace state_machine
