#include <gtest/gtest.h>
#include <state_machine/owner_loop.hpp>

#include <atomic>
#include <chrono>
#include <deque>
#include <functional>
#include <future>
#include <limits>
#include <vector>

namespace {
using namespace std::chrono_literals;
struct Queue {
    std::deque<std::function<void()>> callbacks;
    void dispatchOne() {
        if (callbacks.empty())
            return;
        auto callback = std::move(callbacks.front());
        callbacks.pop_front();
        callback();
    }
    bool empty() const { return callbacks.empty(); }
};
struct Demand {
    double domain;
    double wall;
};
constexpr double absent = std::numeric_limits<double>::infinity();
auto noDeadline() {
    return state_machine::OwnerWake::Deadline{};
}
} // namespace

TEST(OwnerLoop, DispatchesOneCallbackThenWorkAndDemandWithoutStarvation) {
    state_machine::OwnerWake wake;
    state_machine::OwnerLoop loop(wake);
    Queue queue;
    std::vector<int> trace;
    for (int i = 1; i <= 3; ++i)
        queue.callbacks.emplace_back([&, i] {
            trace.push_back(i);
        });
    int turns = 0;
    unsigned projections = 0;
    loop.run(
        queue,
        [&] {
            trace.push_back(++turns * 10);
            if (turns == 3)
                wake.stop();
        },
        [&] {
            trace.push_back(turns * 100);
            return noDeadline();
        },
        [&](const auto& next) {
            ++projections;
            return next;
        },
        [] {
            return true;
        });
    EXPECT_EQ(trace, (std::vector<int>{1, 10, 100, 2, 20, 200, 3, 30, 300}));
    EXPECT_EQ(projections, 1U); // Remaining callbacks continue before waiting/projecting.
}

TEST(OwnerLoop, NotificationDuringWorkSurvivesUntilTheNextTurn) {
    state_machine::OwnerWake wake;
    state_machine::OwnerLoop loop(wake);
    Queue queue;
    std::promise<void> in_work, release;
    auto entered = in_work.get_future();
    auto released = release.get_future();
    unsigned turns = 0;
    auto owner = std::async(std::launch::async, [&] {
        loop.run(
            queue,
            [&] {
                if (++turns == 1) {
                    in_work.set_value();
                    released.wait();
                } else {
                    wake.stop();
                }
            },
            noDeadline,
            [](const auto& next) {
                return next;
            },
            [] {
                return true;
            });
    });
    EXPECT_EQ(entered.wait_for(1s), std::future_status::ready);
    wake.notify();
    release.set_value();
    const auto completed = owner.wait_for(1s);
    wake.stop();
    owner.get();
    EXPECT_EQ(completed, std::future_status::ready);
    EXPECT_EQ(turns, 2U);
}

TEST(OwnerLoop, NotificationBetweenEmptyQueueAndWaitCannotBeLost) {
    state_machine::OwnerWake wake;
    state_machine::OwnerLoop loop(wake);
    Queue queue;
    std::promise<void> projecting, release;
    auto entered = projecting.get_future();
    auto released = release.get_future();
    unsigned turns = 0;
    auto owner = std::async(std::launch::async, [&] {
        loop.run(
            queue,
            [&] {
                if (++turns == 2)
                    wake.stop();
            },
            noDeadline,
            [&](const auto& next) {
                if (turns == 1) {
                    projecting.set_value();
                    released.wait();
                }
                return next;
            },
            [] {
                return true;
            });
    });
    EXPECT_EQ(entered.wait_for(1s), std::future_status::ready);
    wake.notify();
    release.set_value();
    const auto completed = owner.wait_for(1s);
    wake.stop();
    owner.get();
    EXPECT_EQ(completed, std::future_status::ready);
    EXPECT_EQ(turns, 2U);
}

TEST(OwnerLoop, IdleDoesNotPollAndShutdownReleasesTheWait) {
    state_machine::OwnerWake wake;
    state_machine::OwnerLoop loop(wake);
    Queue queue;
    std::atomic<unsigned> turns{0};
    std::promise<void> first_turn;
    auto entered = first_turn.get_future();
    auto owner = std::async(std::launch::async, [&] {
        loop.run(
            queue,
            [&] {
                if (++turns == 1)
                    first_turn.set_value();
            },
            noDeadline,
            [](const auto& next) {
                return next;
            },
            [] {
                return true;
            });
    });
    EXPECT_EQ(entered.wait_for(1s), std::future_status::ready);
    EXPECT_EQ(owner.wait_for(30ms), std::future_status::timeout);
    EXPECT_EQ(turns.load(), 1U);
    wake.stop();
    EXPECT_EQ(owner.wait_for(1s), std::future_status::ready);
    owner.get();
    EXPECT_EQ(turns.load(), 1U);
}

TEST(OwnerWake, SteadyDeadlineWaitIsNotEarly) {
    state_machine::OwnerWake wake;
    const auto observed = wake.observe();
    const auto deadline = std::chrono::steady_clock::now() + 10ms;
    wake.wait(observed, deadline);
    EXPECT_GE(std::chrono::steady_clock::now(), deadline);
}

TEST(OwnerDeadline, PausedAndBackwardDomainCannotAdvanceWithSteadyTime) {
    unsigned domain_reads = 0, steady_reads = 0;
    const auto domain_now = [&] {
        ++domain_reads;
        return 10.0;
    };
    const auto backward_now = [&] {
        ++domain_reads;
        return 9.0;
    };
    const auto due_now = [&] {
        ++domain_reads;
        return 11.0;
    };
    const auto steady_now = [&] {
        ++steady_reads;
        return 200.0;
    };
    EXPECT_FALSE(state_machine::projectOwnerDeadline(Demand{11.0, absent}, true, domain_now, steady_now));
    EXPECT_FALSE(state_machine::projectOwnerDeadline(Demand{11.0, absent}, true, backward_now, steady_now));
    const auto wall = state_machine::projectOwnerDeadline(Demand{11.0, 200.25}, true, backward_now, steady_now);
    ASSERT_TRUE(wall);
    EXPECT_EQ(wall->time_since_epoch(), std::chrono::nanoseconds(200250000000LL));
    EXPECT_EQ(domain_reads, 3U);
    EXPECT_EQ(steady_reads, 0U);
    const auto due = state_machine::projectOwnerDeadline(Demand{11.0, absent}, true, due_now, steady_now);
    ASSERT_TRUE(due);
    EXPECT_EQ(due->time_since_epoch(), 200s);
    EXPECT_EQ(steady_reads, 1U);
}

TEST(OwnerDeadline, MapsOnlyTheRemainingDomainDurationAndCeilsTheWait) {
    unsigned domain_reads = 0, steady_reads = 0;
    const auto domain_now = [&] {
        ++domain_reads;
        return 100.0;
    };
    const auto steady_now = [&] {
        ++steady_reads;
        return 5.0;
    };
    const auto mapped = state_machine::projectOwnerDeadline(Demand{100.25, 9.0}, false, domain_now, steady_now);
    ASSERT_TRUE(mapped);
    EXPECT_EQ(mapped->time_since_epoch(), std::chrono::nanoseconds(5250000000LL));
    EXPECT_EQ(domain_reads, 2U);
    EXPECT_EQ(steady_reads, 1U);
    const auto rounded =
        state_machine::projectOwnerDeadline(Demand{absent, 1.0000000001}, false, domain_now, steady_now);
    ASSERT_TRUE(rounded);
    EXPECT_EQ(rounded->time_since_epoch(), std::chrono::nanoseconds(1000000001));
}

TEST(OwnerDeadline, PreservesExistingNanAndInfinityBehavior) {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const auto now = [] {
        return 1.0;
    };
    EXPECT_FALSE(state_machine::projectOwnerDeadline(Demand{absent, absent}, false, now, now));
    EXPECT_FALSE(state_machine::projectOwnerDeadline(Demand{0.0, nan}, false, now, now));
    const auto result = state_machine::projectOwnerDeadline(Demand{nan, 2.0}, false, now, now);
    ASSERT_TRUE(result);
    EXPECT_EQ(result->time_since_epoch(), 2s);
}
