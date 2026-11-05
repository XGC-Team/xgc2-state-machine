#include <gtest/gtest.h>
#include <limits>
#include <state_machine/ros1_adaptive_loop.hpp>
#include <state_machine/ros1_queued_owner.hpp>
#include <vector>

#include <atomic>
#include <boost/make_shared.hpp>
#include <chrono>
#include <functional>
#include <thread>

namespace {
using namespace std::chrono_literals;
class Callback : public ros::CallbackInterface {
  public:
    explicit Callback(std::function<void()> value) : value_(std::move(value)) {}
    CallResult call() override {
        value_();
        return Success;
    }

  private:
    std::function<void()> value_;
};
template <class Predicate> bool await(Predicate predicate) {
    const auto end = std::chrono::steady_clock::now() + 3s;
    while (!predicate() && std::chrono::steady_clock::now() < end)
        std::this_thread::sleep_for(1ms);
    return predicate();
}
TEST(OwnerLoop, QueuedTurnKeepsCallbackOrderAndContinuation) {
    ros::CallbackQueue queue;
    std::vector<int> trace;
    state_machine::Ros1QueuedTurn* target = nullptr;
    state_machine::Ros1QueuedTurn turn(queue, [&] {
        target->beginTurn();
        trace.push_back(2);
        if (trace.size() == 2)
            target->request();
    });
    target = &turn;
    queue.addCallback(boost::make_shared<Callback>([&] {
        trace.push_back(1);
    }));
    turn.request();
    turn.request();
    queue.addCallback(boost::make_shared<Callback>([&] {
        trace.push_back(3);
    }));
    while (!queue.isEmpty())
        queue.callOne(ros::WallDuration(0));
    EXPECT_EQ(trace, (std::vector<int>{1, 2, 3, 2}));
    turn.request();
    turn.cancel();
    queue.callOne(ros::WallDuration(0));
    EXPECT_EQ(trace.size(), 4U);
}

TEST(OwnerLoop, DomainTimerUsesSameQueueAndReplacesEarlierLaterOrCancelledDemand) {
    ros::CallbackQueue queue;
    ros::NodeHandle nh;
    nh.setCallbackQueue(&queue);
    ros::Time::setNow(ros::Time(10, 0));
    unsigned fired = 0;
    const auto owner = std::this_thread::get_id();
    state_machine::Ros1DomainDeadline<uint64_t> deadline(nh, [&] {
        EXPECT_EQ(std::this_thread::get_id(), owner);
        ++fired;
    });
    const auto quiet = [&] {
        const auto until = std::chrono::steady_clock::now() + 30ms;
        while (std::chrono::steady_clock::now() < until) {
            queue.callOne(ros::WallDuration(0));
            std::this_thread::sleep_for(1ms);
        }
    };
    const auto reached = [&](unsigned count) {
        return await([&] {
            queue.callOne(ros::WallDuration(0));
            return fired == count;
        });
    };
    deadline.arm(11, [] {
        return ros::Duration(1);
    });
    deadline.arm(12, [] {
        return ros::Duration(2);
    });
    ros::Time::setNow(ros::Time(11, 0));
    quiet();
    EXPECT_EQ(fired, 0U);
    ros::Time::setNow(ros::Time(11, 999999999));
    quiet();
    EXPECT_EQ(fired, 0U);
    ros::Time::setNow(ros::Time(12, 0));
    EXPECT_TRUE(reached(1));
    deadline.arm(15, [] {
        return ros::Duration(3);
    });
    deadline.arm(13, [] {
        return ros::Duration(1);
    });
    deadline.arm(13, [] {
        ADD_FAILURE() << "unchanged demand evaluated duration";
        return ros::Duration(9);
    });      // Equal demand cannot delay the active arm.
    quiet(); // Paused external time cannot fire its future domain timer.
    EXPECT_EQ(fired, 1U);
    ros::Time::setNow(ros::Time(13, 0));
    EXPECT_TRUE(reached(2));
    deadline.arm(14, [] {
        return ros::Duration(1);
    });
    deadline.cancel();
    ros::Time::setNow(ros::Time(16, 0));
    quiet();
    EXPECT_EQ(fired, 2U);
}

TEST(OwnerLoop, ClockInputCompletionWallAndShutdownWakeOneSleepingOwner) {
    state_machine::Ros1AdaptiveLoop loop;
    ros::NodeHandle nh;
    loop.attach(nh);
    ASSERT_TRUE(loop.externalClock());
    auto clock = nh.advertise<rosgraph_msgs::Clock>("/clock", 1, true);
    std::atomic<unsigned> turns{0}, inputs{0}, wall_fires{0};
    std::atomic<double> observed_clock{0};
    std::atomic<bool> in_work{false}, release_work{false}, request_barrier{false};
    double wall_deadline = std::numeric_limits<double>::infinity();
    const auto owner = std::this_thread::get_id();
    auto enqueue = [&](std::function<void()> value) {
        loop.queue()->addCallback(boost::make_shared<Callback>(std::move(value)));
    };
    std::thread actor([&] {
        EXPECT_TRUE(await([&] {
            return turns.load() > 0 && clock.getNumSubscribers() >= 1;
        }));
        rosgraph_msgs::Clock message;
        message.clock = ros::Time(123.0);
        clock.publish(message);
        EXPECT_TRUE(await([&] {
            return observed_clock.load() == 123.0;
        }));
        std::this_thread::sleep_for(20ms);
        const auto paused = turns.load();
        std::this_thread::sleep_for(80ms);
        EXPECT_EQ(turns.load(), paused) << "paused domain deadline must not poll";
        enqueue([&] {
            EXPECT_EQ(std::this_thread::get_id(), owner);
            ++inputs;
            wall_deadline = loop.wallNow() + .04;
        });
        EXPECT_TRUE(await([&] {
            return inputs.load() == 1;
        }));
        EXPECT_TRUE(await([&] {
            return wall_fires.load() == 1;
        }));
        const auto fired = turns.load();
        std::this_thread::sleep_for(40ms);
        EXPECT_EQ(turns.load(), fired);
        // A completion while the owner is still working must remain latched across its wait.
        request_barrier = true;
        loop.notify();
        EXPECT_TRUE(await([&] {
            return in_work.load();
        }));
        const auto during = turns.load();
        loop.notify();
        release_work = true;
        EXPECT_TRUE(await([&] {
            return turns.load() > during;
        }));
        ros::requestShutdown();
    });
    loop.run(
        [&] {
            ++turns;
            observed_clock = loop.now();
            if (loop.wallNow() >= wall_deadline) {
                ++wall_fires;
                wall_deadline = std::numeric_limits<double>::infinity();
            }
            if (request_barrier.exchange(false)) {
                in_work = true;
                while (!release_work.load())
                    std::this_thread::yield();
            }
        },
        [&] {
            struct Demand {
                double domain;
                double wall;
            };
            return Demand{124.0, wall_deadline};
        });
    actor.join();
    EXPECT_EQ(inputs.load(), 1U);
    EXPECT_EQ(wall_fires.load(), 1U);
    EXPECT_LT(turns.load(), 12U);
}
} // namespace
int main(int argc, char** argv) {
    ros::init(argc, argv, "foundation_owner_loop_test");
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
