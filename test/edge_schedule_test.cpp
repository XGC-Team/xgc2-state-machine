#include <gtest/gtest.h>
#include <state_machine/owner_schedule.hpp>

#include <cstdint>
#include <deque>
#include <functional>
#include <limits>
#include <optional>
#include <stdexcept>
#include <vector>

TEST(DeadlineArm, ReplacesCancelsAndRearmsConsumedOneShot) {
    state_machine::DeadlineArm<int64_t> schedule;
    std::vector<std::optional<int64_t>> calls;
    const auto wake_at = [&](int64_t at) {
        calls.emplace_back(at);
    };
    const auto cancel = [&] {
        calls.emplace_back(std::nullopt);
    };
    schedule.update(std::nullopt, wake_at, cancel);
    schedule.update(100, wake_at, cancel);
    schedule.update(100, wake_at, cancel); // Same demand does not restart a phase.
    schedule.update(50, wake_at, cancel);
    schedule.update(200, wake_at, cancel); // Later replaces earlier, not a stale minimum.
    schedule.update(std::nullopt, wake_at, cancel);
    schedule.update(std::nullopt, wake_at, cancel);
    schedule.update(200, wake_at, cancel);
    schedule.consumed();
    schedule.update(200, wake_at, cancel); // A consumed one-shot must be armed again.
    schedule.update(std::nullopt, wake_at, cancel);
    EXPECT_EQ(calls, (std::vector<std::optional<int64_t>>{100, 50, 200, std::nullopt, 200, 200, std::nullopt}));
}

TEST(DeadlineArm, EqualityIsLazyAndFailedInstallationKeepsThePreviousArm) {
    state_machine::DeadlineArm<uint64_t> deadline;
    const uint64_t first = (uint64_t{1} << 60) + 1;
    unsigned installs = 0;
    const auto install = [&](uint64_t) {
        ++installs;
    };
    deadline.arm(first, install);
    deadline.arm(first, [](uint64_t) {
        throw std::runtime_error("equal target evaluated conversion");
    });
    EXPECT_THROW(deadline.arm(first + 1,
                              [](uint64_t) {
                                  throw std::runtime_error("conversion/installation");
                              }),
                 std::runtime_error);
    deadline.arm(first, install);
    EXPECT_EQ(installs, 1U);
    deadline.arm(first + 1, install); // The adjacent nanosecond is not rounded through double.
    EXPECT_EQ(installs, 2U);
    EXPECT_THROW(deadline.cancel([] {
        throw std::runtime_error("cancel");
    }),
                 std::runtime_error);
    deadline.arm(first + 1, install);
    EXPECT_EQ(installs, 2U);
    deadline.consumed();
    deadline.arm(first + 1, install);
    EXPECT_EQ(installs, 3U);
}

TEST(DeadlineArm, FloatingEqualityRetainsOriginalZeroAndNanSemantics) {
    state_machine::DeadlineArm<double> deadline;
    unsigned installs = 0;
    const auto install = [&](double) {
        ++installs;
    };
    deadline.arm(0.0, install);
    deadline.arm(-0.0, install);
    const double nan = std::numeric_limits<double>::quiet_NaN();
    deadline.arm(nan, install);
    deadline.arm(nan, install);
    EXPECT_EQ(installs, 3U);
}

TEST(QueuedTurn, KeepsCallbackOrderCoalescingAndContinuation) {
    state_machine::QueuedTurn turn;
    std::deque<std::function<void()>> queue;
    std::vector<int> trace;
    std::function<void()> callback;
    const auto post = [&] {
        queue.push_back(callback);
    };
    callback = [&] {
        turn.beginTurn();
        trace.push_back(2);
        if (trace.size() == 2)
            turn.request(post);
    };
    queue.emplace_back([&] {
        trace.push_back(1);
    });
    turn.request(post);
    turn.request(post);
    queue.emplace_back([&] {
        trace.push_back(3);
    });
    while (!queue.empty()) {
        auto next = std::move(queue.front());
        queue.pop_front();
        next();
    }
    EXPECT_EQ(trace, (std::vector<int>{1, 2, 3, 2}));
    turn.request(post);
    turn.cancel([&] {
        queue.clear();
    });
    turn.request(post); // Teardown removal did not introduce a restart contract.
    EXPECT_TRUE(queue.empty());
}

TEST(QueuedTurn, FailedEnqueueKeepsTheOriginalPendingSemantics) {
    state_machine::QueuedTurn turn;
    EXPECT_THROW(turn.request([] {
        throw std::runtime_error("enqueue");
    }),
                 std::runtime_error);
    unsigned calls = 0;
    turn.request([&] {
        ++calls;
    });
    EXPECT_EQ(calls, 0U);
    turn.beginTurn();
    turn.request([&] {
        ++calls;
    });
    EXPECT_EQ(calls, 1U);
}
